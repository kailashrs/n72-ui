// SPDX-License-Identifier: MIT
/*
 * LVGL on the iPod touch 2G's PowerVR MBX Lite (2D engine, /dev/mbx).
 *
 * - All LVGL draw buffers come from one GPU-mapped, CPU-cached pool, so the
 *   GPU can target any of them; cache maintenance goes through MBX_IOC_SYNC.
 * - A draw unit takes plain opaque fills (no radius, no gradient).
 * - The display renders into a full-screen pool buffer ("direct" mode) and
 *   the GPU copies the refreshed areas to the framebuffer.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/apple-mbx.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/lvgl_private.h"
#include "mbx_gpu.h"

#define DRAW_UNIT_ID_MBX	5
#define POOL_SIZE		(3 * 1024 * 1024)
#define POOL_ALIGN		64
#define POOL_BLOCKS		256

static int mbx = -1;
static struct mbx_fb_info fb;
static uint32_t pool_handle, pool_gpu;
static uint8_t *pool;

struct mbx_gpu_stats mbx_gpu_stats;

/* ---------------------------------------------------- pool allocator */

/* First-fit over a small table of blocks, kept sorted by offset. */
static struct {
	uint32_t off, size;
	bool used;
} blocks[POOL_BLOCKS];
static int nblocks;

static void *pool_alloc(size_t size)
{
	int i;

	size = (size + POOL_ALIGN - 1) & ~(size_t)(POOL_ALIGN - 1);
	for (i = 0; i < nblocks; i++) {
		if (blocks[i].used || blocks[i].size < size)
			continue;
		if (blocks[i].size > size && nblocks < POOL_BLOCKS) {
			memmove(&blocks[i + 2], &blocks[i + 1],
				(nblocks - i - 1) * sizeof(blocks[0]));
			blocks[i + 1].off = blocks[i].off + size;
			blocks[i + 1].size = blocks[i].size - size;
			blocks[i + 1].used = false;
			blocks[i].size = size;
			nblocks++;
		}
		blocks[i].used = true;
		return pool + blocks[i].off;
	}
	return NULL;
}

static void pool_free(void *p)
{
	uint32_t off = (uint8_t *)p - pool;
	int i;

	for (i = 0; i < nblocks; i++)
		if (blocks[i].off == off)
			break;
	if (i == nblocks)
		return;
	blocks[i].used = false;
	if (i + 1 < nblocks && !blocks[i + 1].used) {	/* merge right */
		blocks[i].size += blocks[i + 1].size;
		memmove(&blocks[i + 1], &blocks[i + 2], (nblocks - i - 2) * sizeof(blocks[0]));
		nblocks--;
	}
	if (i > 0 && !blocks[i - 1].used) {		/* merge left */
		blocks[i - 1].size += blocks[i].size;
		memmove(&blocks[i], &blocks[i + 1], (nblocks - i - 1) * sizeof(blocks[0]));
		nblocks--;
	}
}

static bool in_pool(const void *p)
{
	return p && (const uint8_t *)p >= pool && (const uint8_t *)p < pool + POOL_SIZE;
}

static uint32_t gpu_addr(const void *p)
{
	return pool_gpu + ((const uint8_t *)p - pool);
}

/* ---------------------------------------------------------- the GPU */

static void sync_range(const void *p, uint32_t size, uint32_t op)
{
	struct mbx_sync s = {
		.handle = pool_handle, .op = op,
		.offset = (const uint8_t *)p - pool, .size = size,
	};

	ioctl(mbx, MBX_IOC_SYNC, &s);
}

static int submit(const uint32_t *w, int n)
{
	struct mbx_submit s = { .words = (uintptr_t)w, .count = n };

	mbx_gpu_stats.submits++;
	return ioctl(mbx, MBX_IOC_SUBMIT, &s);
}

static int put_tail(uint32_t *w, int n)
{
	int i;

	for (i = 0; i < 6; i++)
		w[n++] = 0x70000000;
	return n;
}

static int put_fill(uint32_t *w, uint32_t dst, uint32_t stride, uint32_t color,
		    int x, int y, int width, int height)
{
	int n = 0;

	w[n++] = 0xa0000000 | MBX_FMT_ARGB8888 | (stride & 0x7fff);
	w[n++] = dst;
	w[n++] = 0x94000000 | MBX_FMT_ARGB8888 | (stride & 0x7fff);
	w[n++] = 0;
	w[n++] = 0x30000000;
	w[n++] = 0x60800200;
	w[n++] = 0x8000f0f0;			/* ROP 0xf0: pattern (colour) copy */
	w[n++] = color;
	w[n++] = ((uint32_t)x << 16 & 0x1fff0000) | (y & 0x1fff);
	w[n++] = ((uint32_t)(x + width) << 16 & 0x1fff0000) | ((y + height) & 0x1fff);
	return put_tail(w, n);
}

static int put_copy(uint32_t *w, uint32_t dst, uint32_t dstride, uint32_t src,
		    uint32_t sstride, int sx, int sy, int dx, int dy, int width,
		    int height)
{
	int n = 0;

	w[n++] = 0xa0000000 | MBX_FMT_ARGB8888 | (dstride & 0x7fff);
	w[n++] = dst;
	w[n++] = 0x94000000 | MBX_FMT_ARGB8888 | (sstride & 0x7fff);
	w[n++] = src;
	w[n++] = 0x30000000 | ((uint32_t)sx << 14 & 0x7ffc000) | (sy & 0x1fff);
	w[n++] = 0x60800200;			/* 1:1 */
	w[n++] = 0x8000cccc;			/* ROP 0xcc: source copy */
	w[n++] = 0xffffffff;
	w[n++] = ((uint32_t)dx << 16 & 0x1fff0000) | (dy & 0x1fff);
	w[n++] = ((uint32_t)(dx + width) << 16 & 0x1fff0000) | ((dy + height) & 0x1fff);
	return put_tail(w, n);
}

/* ------------------------------------------------- LVGL draw buffers */

static void *buf_malloc(size_t size, lv_color_format_t cf)
{
	void *p;

	LV_UNUSED(cf);
	p = pool_alloc(size);
	if (!p) {
		mbx_gpu_stats.pool_misses++;
		p = lv_malloc(size);		/* CPU-only fallback */
	}
	return p;
}

static void buf_free(void *p)
{
	if (in_pool(p))
		pool_free(p);
	else
		lv_free(p);
}

static void buf_sync_area(const lv_draw_buf_t *b, const lv_area_t *a, uint32_t op)
{
	uint32_t stride = b->header.stride;

	if (!in_pool(b->data))
		return;
	sync_range(b->data + a->y1 * stride, (lv_area_get_height(a)) * stride, op);
}

static void buf_invalidate(const lv_draw_buf_t *b, const lv_area_t *a)
{
	buf_sync_area(b, a, MBX_SYNC_FLUSH);
}

static void buf_clean(const lv_draw_buf_t *b, const lv_area_t *a)
{
	buf_sync_area(b, a, MBX_SYNC_CLEAN);
}

/* --------------------------------------------------- the draw unit */

static bool cf_ok(lv_color_format_t cf)
{
	return cf == LV_COLOR_FORMAT_XRGB8888 || cf == LV_COLOR_FORMAT_ARGB8888;
}

static int32_t mbx_evaluate(lv_draw_unit_t *u, lv_draw_task_t *t)
{
	const lv_draw_dsc_base_t *base = t->draw_dsc;

	LV_UNUSED(u);
	if (t->type != LV_DRAW_TASK_TYPE_FILL || !cf_ok(base->layer->color_format))
		return 0;

	const lv_draw_fill_dsc_t *d = t->draw_dsc;

	if (d->radius != 0 || d->grad.dir != LV_GRAD_DIR_NONE || d->opa < LV_OPA_MAX)
		return 0;
	/*
	 * A GPU fill costs two ioctls and an interrupt (~0.5 ms); the CPU fills
	 * about 40 Mpixel/s, so only large areas are worth sending.
	 */
	if (lv_area_get_size(&t->area) < 16384)
		return 0;
	if (t->preference_score > 50) {
		t->preference_score = 50;
		t->preferred_draw_unit_id = DRAW_UNIT_ID_MBX;
	}
	return 1;
}

static void mbx_fill(lv_draw_unit_t *u, lv_draw_task_t *t)
{
	lv_layer_t *layer = u->target_layer;
	lv_draw_buf_t *b = layer->draw_buf;
	const lv_draw_fill_dsc_t *d = t->draw_dsc;
	lv_area_t a;
	uint32_t w[16];

	if (!lv_area_intersect(&a, &t->area, u->clip_area))
		return;
	if (!in_pool(b->data)) {
		lv_draw_sw_fill(u, (lv_draw_fill_dsc_t *)d, &t->area);
		return;
	}
	lv_area_move(&a, -layer->buf_area.x1, -layer->buf_area.y1);
	buf_invalidate(b, &a);			/* drop CPU lines over the area */
	submit(w, put_fill(w, gpu_addr(b->data), b->header.stride,
			   0xff000000 | lv_color_to_u32(d->color) ,
			   a.x1, a.y1, lv_area_get_width(&a), lv_area_get_height(&a)));
	mbx_gpu_stats.fills++;
}

static int32_t mbx_dispatch(lv_draw_unit_t *u, lv_layer_t *layer)
{
	lv_draw_task_t *t = lv_draw_get_next_available_task(layer, NULL, DRAW_UNIT_ID_MBX);

	if (!t || t->preferred_draw_unit_id != DRAW_UNIT_ID_MBX)
		return LV_DRAW_UNIT_IDLE;
	if (!lv_draw_layer_alloc_buf(layer))
		return LV_DRAW_UNIT_IDLE;

	t->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
	u->target_layer = layer;
	u->clip_area = &t->clip_area;
	mbx_fill(u, t);
	t->state = LV_DRAW_TASK_STATE_READY;
	lv_draw_dispatch_request();
	return 1;
}

/* -------------------------------------------------------- display */

static uint8_t *disp_buf;
static uint32_t flush_words[16 * 64];
static int flush_n;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
	uint32_t stride = fb.stride;

	LV_UNUSED(px_map);
	sync_range(disp_buf + area->y1 * stride, lv_area_get_height(area) * stride,
		   MBX_SYNC_CLEAN);
	if (flush_n > (int)(sizeof(flush_words) / 4) - 16) {
		submit(flush_words, flush_n);
		flush_n = 0;
	}
	flush_n += put_copy(flush_words + flush_n, fb.gpu_addr, fb.stride,
			    gpu_addr(disp_buf), stride, area->x1, area->y1,
			    area->x1, area->y1, lv_area_get_width(area),
			    lv_area_get_height(area));
	mbx_gpu_stats.flushes++;
	if (lv_display_flush_is_last(disp) && flush_n) {
		submit(flush_words, flush_n);
		flush_n = 0;
	}
	lv_display_flush_ready(disp);
}

lv_display_t *mbx_gpu_display_create(void)
{
	struct mbx_alloc a = { .size = POOL_SIZE, .flags = MBX_ALLOC_CACHED };
	lv_draw_buf_handlers_t *h;
	lv_draw_unit_t *u;
	lv_display_t *disp;
	uint32_t size;

	mbx = open("/dev/mbx", O_RDWR | O_CLOEXEC);
	if (mbx < 0)
		return NULL;
	if (ioctl(mbx, MBX_IOC_FB_INFO, &fb) || ioctl(mbx, MBX_IOC_ALLOC, &a))
		goto fail;
	pool = mmap(NULL, POOL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mbx,
		    a.mmap_offset);
	if (pool == MAP_FAILED)
		goto fail;
	pool_handle = a.handle;
	pool_gpu = a.gpu_addr;
	blocks[0].off = 0;
	blocks[0].size = POOL_SIZE;
	nblocks = 1;

	h = lv_draw_buf_get_handlers();
	h->buf_malloc_cb = buf_malloc;
	h->buf_free_cb = buf_free;
	h->invalidate_cache_cb = buf_invalidate;
	h->flush_cache_cb = buf_clean;

	u = lv_draw_create_unit(sizeof(lv_draw_unit_t));
	u->evaluate_cb = mbx_evaluate;
	u->dispatch_cb = mbx_dispatch;

	size = fb.stride * fb.height;
	disp_buf = pool_alloc(size);
	if (!disp_buf)
		goto fail;
	disp = lv_display_create(fb.width, fb.height);
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
	lv_display_set_buffers(disp, disp_buf, NULL, size, LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(disp, flush_cb);
	printf("n72-ui: MBX GPU: framebuffer at GPU %08x, %u KiB draw pool at %08x\n",
	       fb.gpu_addr, POOL_SIZE / 1024, pool_gpu);
	return disp;

fail:
	perror("n72-ui: /dev/mbx");
	close(mbx);
	mbx = -1;
	return NULL;
}

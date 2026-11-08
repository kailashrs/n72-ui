// SPDX-License-Identifier: MIT
/*
 * n72-ui: a small LVGL launcher for Linux on the iPod touch 2G (N72AP).
 *
 * Display: /dev/fb0 (simplefb, 320x480 XRGB8888). Touch: the Zephyr2
 * evdev device. Buttons: gpio-keys (Home returns to the launcher, Power
 * blanks the screen, the volume keys change the brightness).
 *
 * Environment:
 *   N72_FB         framebuffer device (default /dev/fb0)
 *   N72_TOUCH      touch event device (default: found by name)
 *   N72_KEYS       button event device (default: found by name)
 *   N72_TOUCH_CAL  "min_x min_y max_x max_y" raw touch range; swap min and
 *                  max to flip an axis (default: the device's own range)
 *   N72_TOUCH_SWAP set to 1 to swap the touch axes
 *   N72_GPU        set to 0 to draw with the CPU only (default: MBX GPU
 *                  through /dev/mbx when present)
 *
 * n72-ui --bench times full-screen redraws and exits.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "lvgl/demos/lv_demos.h"
#include "mbx_gpu.h"

#define W 320
#define H 480

static char *self_argv[4];
static lv_obj_t *home_scr;
static lv_obj_t *clock_lbl, *date_lbl;
static bool in_demo;
static int keys_fd = -1;
static int tty_fd = -1;

/* ---------------------------------------------------------------- utils */

static uint32_t tick_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int read_int(const char *path, int def)
{
	FILE *f = fopen(path, "r");
	int v = def;

	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = def;
		fclose(f);
	}
	return v;
}

static void write_int(const char *path, int v)
{
	FILE *f = fopen(path, "w");

	if (f) {
		fprintf(f, "%d\n", v);
		fclose(f);
	}
}

/* Find /dev/input/eventN whose device name contains @name. */
static const char *find_input(const char *name)
{
	static char found[2][64];
	static int slot;
	char path[128], devname[128];
	int i;

	for (i = 0; i < 16; i++) {
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		devname[0] = 0;
		ioctl(fd, EVIOCGNAME(sizeof(devname)), devname);
		close(fd);
		if (strstr(devname, name)) {
			char *out = found[slot++ & 1];

			snprintf(out, 64, "%s", path);
			return out;
		}
	}
	return NULL;
}

/* ------------------------------------------------------------ backlight */

static char bl_dir[128];
static int bl_max = 1;
static bool screen_off;

static void backlight_init(void)
{
	DIR *d = opendir("/sys/class/backlight");
	struct dirent *e;

	if (!d)
		return;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(bl_dir, sizeof(bl_dir), "/sys/class/backlight/%s",
			 e->d_name);
		break;
	}
	closedir(d);
	if (bl_dir[0]) {
		char p[160];

		snprintf(p, sizeof(p), "%s/max_brightness", bl_dir);
		bl_max = read_int(p, 1);
	}
}

/* Brightness in percent. */
static int backlight_get(void)
{
	char p[160];

	if (!bl_dir[0])
		return 100;
	snprintf(p, sizeof(p), "%s/brightness", bl_dir);
	return read_int(p, bl_max) * 100 / bl_max;
}

static void backlight_set(int pct)
{
	char p[160];

	if (!bl_dir[0])
		return;
	pct = LV_CLAMP(5, pct, 100);
	snprintf(p, sizeof(p), "%s/brightness", bl_dir);
	write_int(p, pct * bl_max / 100);
}

static void backlight_power(bool on)
{
	char p[160];

	if (!bl_dir[0])
		return;
	snprintf(p, sizeof(p), "%s/bl_power", bl_dir);
	write_int(p, on ? 0 : 4);
}

/* -------------------------------------------------------------- helpers */

static void go_home(void)
{
	if (in_demo) {
		/* The demos keep global state and timers: start afresh. */
		if (tty_fd >= 0)
			close(tty_fd);
		execv("/proc/self/exe", self_argv);
		return;
	}
	if (lv_screen_active() != home_scr)
		lv_screen_load_anim(home_scr, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 220, 0,
				    true);
}

static void back_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	go_home();
}

static void style_screen(lv_obj_t *scr)
{
	lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f1021), 0);
	lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x2a1747), 0);
	lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(scr, lv_color_white(), 0);
}

/* A screen with a header (back button and title); returns the content area. */
static lv_obj_t *app_screen(const char *title, lv_obj_t **scr_out)
{
	lv_obj_t *scr = lv_obj_create(NULL);
	lv_obj_t *hdr, *btn, *lbl, *body;

	style_screen(scr);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	hdr = lv_obj_create(scr);
	lv_obj_set_size(hdr, W, 48);
	lv_obj_set_style_bg_color(hdr, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(hdr, LV_OPA_40, 0);
	lv_obj_set_style_border_width(hdr, 0, 0);
	lv_obj_set_style_radius(hdr, 0, 0);
	lv_obj_set_style_pad_all(hdr, 4, 0);
	lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

	btn = lv_button_create(hdr);
	lv_obj_set_height(btn, 38);
	lv_obj_align(btn, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, NULL);
	lbl = lv_label_create(btn);
	lv_label_set_text(lbl, LV_SYMBOL_LEFT " Home");
	lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
	lv_obj_center(lbl);

	lbl = lv_label_create(hdr);
	lv_label_set_text(lbl, title);
	lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
	lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

	body = lv_obj_create(scr);
	lv_obj_set_size(body, W, H - 48);
	lv_obj_set_pos(body, 0, 48);
	lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_radius(body, 0, 0);
	lv_obj_set_style_pad_all(body, 0, 0);

	*scr_out = scr;
	return body;
}

static void show(lv_obj_t *scr)
{
	lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
}

static lv_obj_t *card(lv_obj_t *parent)
{
	lv_obj_t *c = lv_obj_create(parent);

	lv_obj_set_width(c, lv_pct(100));
	lv_obj_set_height(c, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(c, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_10, 0);
	lv_obj_set_style_border_width(c, 0, 0);
	lv_obj_set_style_radius(c, 16, 0);
	lv_obj_set_style_pad_all(c, 14, 0);
	lv_obj_set_style_text_color(c, lv_color_white(), 0);
	return c;
}

/* ---------------------------------------------------------------- paint */

#define PAINT_H (H - 48)
static lv_obj_t *paint_canvas;
static void *paint_buf;
static lv_point_t paint_last;
static bool paint_down;
static uint32_t paint_hue;

static void paint_clear(void)
{
	lv_canvas_fill_bg(paint_canvas, lv_color_hex(0x101018), LV_OPA_COVER);
}

static void paint_event(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	lv_draw_line_dsc_t d;
	lv_layer_t layer;
	lv_area_t a;
	lv_point_t p;

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		paint_down = false;
		return;
	}
	if (!indev || (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING))
		return;
	lv_indev_get_point(indev, &p);
	lv_obj_get_coords(paint_canvas, &a);
	p.x -= a.x1;
	p.y -= a.y1;
	if (code == LV_EVENT_PRESSED || !paint_down) {
		paint_last = p;
		paint_down = true;
		paint_hue = (paint_hue + 47) % 360;
	}

	lv_canvas_init_layer(paint_canvas, &layer);
	lv_draw_line_dsc_init(&d);
	d.color = lv_color_hsv_to_rgb(paint_hue, 75, 100);
	d.width = 10;
	d.round_start = 1;
	d.round_end = 1;
	d.p1.x = paint_last.x;
	d.p1.y = paint_last.y;
	d.p2.x = p.x;
	d.p2.y = p.y;
	lv_draw_line(&layer, &d);
	lv_canvas_finish_layer(paint_canvas, &layer);
	paint_last = p;
}

static void paint_clear_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	paint_clear();
}

static void paint_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	free(paint_buf);
	paint_buf = NULL;
}

static void open_paint(lv_event_t *e)
{
	lv_obj_t *scr, *body, *btn, *lbl;

	LV_UNUSED(e);
	body = app_screen("Paint", &scr);
	lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

	paint_buf = malloc(LV_CANVAS_BUF_SIZE(W, PAINT_H, 32,
					      LV_DRAW_BUF_STRIDE_ALIGN));
	if (!paint_buf)
		return;
	paint_canvas = lv_canvas_create(body);
	lv_canvas_set_buffer(paint_canvas, paint_buf, W, PAINT_H,
			     LV_COLOR_FORMAT_XRGB8888);
	lv_obj_add_flag(paint_canvas, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(paint_canvas, paint_event, LV_EVENT_ALL, NULL);
	lv_obj_add_event_cb(scr, paint_delete_cb, LV_EVENT_DELETE, NULL);
	paint_clear();

	btn = lv_button_create(lv_obj_get_child(scr, 0));
	lv_obj_set_height(btn, 36);
	lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_add_event_cb(btn, paint_clear_cb, LV_EVENT_CLICKED, NULL);
	lbl = lv_label_create(btn);
	lv_label_set_text(lbl, LV_SYMBOL_TRASH);
	lv_obj_center(lbl);

	show(scr);
}

/* ------------------------------------------------------------- settings */

static lv_obj_t *bright_lbl;

static void bright_cb(lv_event_t *e)
{
	lv_obj_t *s = lv_event_get_target(e);
	int v = lv_slider_get_value(s);

	backlight_set(v);
	lv_label_set_text_fmt(bright_lbl, "Brightness  %d%%", v);
}

static void open_settings(lv_event_t *e)
{
	lv_obj_t *scr, *body, *c, *s, *lbl;

	LV_UNUSED(e);
	body = app_screen("Settings", &scr);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(body, 12, 0);
	lv_obj_set_style_pad_row(body, 12, 0);

	c = card(body);
	bright_lbl = lv_label_create(c);
	lv_label_set_text_fmt(bright_lbl, "Brightness  %d%%", backlight_get());
	s = lv_slider_create(c);
	lv_obj_set_width(s, lv_pct(95));
	lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 34);
	lv_slider_set_range(s, 5, 100);
	lv_slider_set_value(s, backlight_get(), LV_ANIM_OFF);
	lv_obj_add_event_cb(s, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_set_height(c, 80);

	c = card(body);
	lbl = lv_label_create(c);
	lv_obj_set_width(lbl, lv_pct(100));
	lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
	lv_label_set_text(lbl,
		LV_SYMBOL_HOME "  Home: back to the launcher\n"
		LV_SYMBOL_POWER "  Power: screen off / on\n"
		LV_SYMBOL_PLUS " " LV_SYMBOL_MINUS "  Volume: brightness");

	show(scr);
}

/* --------------------------------------------------------------- system */

static lv_obj_t *sys_val[5];
static const char *const sys_titles[5] = { "Device", "Kernel", "Uptime", "Memory", "Load" };
static lv_timer_t *sys_timer;

static void sys_update(lv_timer_t *t)
{
	char kernel[160] = "?", line[128];
	long mem_total = 0, mem_avail = 0;
	struct utsname u;
	struct sysinfo si;
	double load = 0;
	FILE *f;

	LV_UNUSED(t);
	if (!uname(&u))
		snprintf(kernel, sizeof(kernel), "%s %s", u.sysname, u.release);
	sysinfo(&si);
	f = fopen("/proc/meminfo", "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			sscanf(line, "MemTotal: %ld", &mem_total);
			sscanf(line, "MemAvailable: %ld", &mem_avail);
		}
		fclose(f);
	}
	f = fopen("/proc/loadavg", "r");
	if (f) {
		if (fscanf(f, "%lf", &load) != 1)
			load = 0;
		fclose(f);
	}
	lv_label_set_text(sys_val[0], "iPod touch 2G (N72AP)\nS5L8720, ARM1176 @ 532 MHz");
	lv_label_set_text(sys_val[1], kernel);
	lv_label_set_text_fmt(sys_val[2], "%ldh %02ldm %02lds", si.uptime / 3600,
			      si.uptime / 60 % 60, si.uptime % 60);
	lv_label_set_text_fmt(sys_val[3], "%ld MiB free of %ld MiB",
			      mem_avail / 1024, mem_total / 1024);
	lv_label_set_text_fmt(sys_val[4], "%d.%02d    %d processes", (int)load,
			      (int)(load * 100) % 100, si.procs);
}

static void sys_delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (sys_timer)
		lv_timer_delete(sys_timer);
	sys_timer = NULL;
}

static void open_system(lv_event_t *e)
{
	lv_obj_t *scr, *body, *c, *t;
	int i;

	LV_UNUSED(e);
	body = app_screen("System", &scr);
	lv_obj_set_style_pad_all(body, 12, 0);
	c = card(body);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(c, 4, 0);
	for (i = 0; i < 5; i++) {
		t = lv_label_create(c);
		lv_label_set_text(t, sys_titles[i]);
		lv_obj_set_style_text_color(t, lv_color_hex(0x8ab4ff), 0);
		if (i)
			lv_obj_set_style_pad_top(t, 10, 0);
		sys_val[i] = lv_label_create(c);
		lv_obj_set_width(sys_val[i], lv_pct(100));
		lv_label_set_long_mode(sys_val[i], LV_LABEL_LONG_WRAP);
		lv_obj_set_style_text_font(sys_val[i], &lv_font_montserrat_16, 0);
	}
	sys_update(NULL);
	sys_timer = lv_timer_create(sys_update, 1000, NULL);
	lv_obj_add_event_cb(scr, sys_delete_cb, LV_EVENT_DELETE, NULL);
	show(scr);
}

/* ---------------------------------------------------------------- power */

static void power_do(lv_event_t *e)
{
	intptr_t what = (intptr_t)lv_event_get_user_data(e);
	lv_obj_t *mbox;

	if (!what) {
		mbox = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target(e)));
		lv_msgbox_close_async(mbox);
		return;
	}
	sync();
	reboot(what == 1 ? RB_POWER_OFF : RB_AUTOBOOT);
}

static void open_power(lv_event_t *e)
{
	lv_obj_t *mbox, *btn;

	LV_UNUSED(e);
	mbox = lv_msgbox_create(NULL);
	lv_msgbox_add_title(mbox, "Power");
	lv_msgbox_add_text(mbox, "Power off or restart the iPod?\n"
			   "Restarting goes back to the bootloader.");
	btn = lv_msgbox_add_footer_button(mbox, "Off");
	lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_RED), 0);
	lv_obj_add_event_cb(btn, power_do, LV_EVENT_CLICKED, (void *)1);
	btn = lv_msgbox_add_footer_button(mbox, "Restart");
	lv_obj_add_event_cb(btn, power_do, LV_EVENT_CLICKED, (void *)2);
	btn = lv_msgbox_add_footer_button(mbox, "Cancel");
	lv_obj_add_event_cb(btn, power_do, LV_EVENT_CLICKED, (void *)0);
}

/* ---------------------------------------------------------------- demos */

static void open_demo(lv_event_t *e)
{
	intptr_t which = (intptr_t)lv_event_get_user_data(e);
	lv_obj_t *scr = lv_obj_create(NULL);

	lv_screen_load(scr);
	in_demo = true;
	if (which == 1)
		lv_demo_widgets();
	else
		lv_demo_music();
}

/* ----------------------------------------------------------------- home */

static void clock_update(lv_timer_t *t)
{
	static const char *const days[] = { "Sunday", "Monday", "Tuesday",
		"Wednesday", "Thursday", "Friday", "Saturday" };
	static const char *const months[] = { "January", "February", "March",
		"April", "May", "June", "July", "August", "September",
		"October", "November", "December" };
	time_t now = time(NULL);
	struct tm tm;

	LV_UNUSED(t);
	localtime_r(&now, &tm);
	lv_label_set_text_fmt(clock_lbl, "%02d:%02d", tm.tm_hour, tm.tm_min);
	if (tm.tm_year + 1900 < 2024)
		lv_label_set_text(date_lbl, "Clock not set");
	else
		lv_label_set_text_fmt(date_lbl, "%s, %d %s", days[tm.tm_wday],
				      tm.tm_mday, months[tm.tm_mon]);
}

struct app {
	const char *name;
	const char *icon;
	uint32_t c1, c2;
	lv_event_cb_t cb;
	intptr_t arg;
};

static const struct app apps[] = {
	{ "Widgets",  LV_SYMBOL_LIST,     0x4f8cff, 0x2b54d6, open_demo, 1 },
	{ "Music",    LV_SYMBOL_AUDIO,    0xff5f8f, 0xc2185b, open_demo, 2 },
	{ "Paint",    LV_SYMBOL_EDIT,     0xffb84d, 0xe67e00, open_paint, 0 },
	{ "Settings", LV_SYMBOL_SETTINGS, 0x9aa5b8, 0x5b6578, open_settings, 0 },
	{ "System",   LV_SYMBOL_EYE_OPEN, 0x45d6a8, 0x14946b, open_system, 0 },
	{ "Power",    LV_SYMBOL_POWER,    0xff6b5b, 0xc23021, open_power, 0 },
};

static void build_home(void)
{
	lv_obj_t *grid, *lbl;
	unsigned int i;

	home_scr = lv_obj_create(NULL);
	style_screen(home_scr);
	lv_obj_remove_flag(home_scr, LV_OBJ_FLAG_SCROLLABLE);

	lbl = lv_label_create(home_scr);
	lv_label_set_text(lbl, "iPod touch  " LV_SYMBOL_USB);
	lv_obj_set_style_text_opa(lbl, LV_OPA_70, 0);
	lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 12, 8);

	clock_lbl = lv_label_create(home_scr);
	lv_obj_set_style_text_font(clock_lbl, &lv_font_montserrat_48, 0);
	lv_obj_align(clock_lbl, LV_ALIGN_TOP_MID, 0, 44);
	date_lbl = lv_label_create(home_scr);
	lv_obj_set_style_text_font(date_lbl, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_opa(date_lbl, LV_OPA_70, 0);
	lv_obj_align(date_lbl, LV_ALIGN_TOP_MID, 0, 104);
	clock_update(NULL);
	lv_timer_create(clock_update, 1000, NULL);

	grid = lv_obj_create(home_scr);
	lv_obj_set_size(grid, W, 300);
	lv_obj_align(grid, LV_ALIGN_BOTTOM_MID, 0, -24);
	lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(grid, 0, 0);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY,
			      LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(grid, 18, 0);
	lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

	for (i = 0; i < sizeof(apps) / sizeof(apps[0]); i++) {
		const struct app *a = &apps[i];
		lv_obj_t *cell, *btn, *icon, *name;

		cell = lv_obj_create(grid);
		lv_obj_set_size(cell, 88, 116);
		lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(cell, 0, 0);
		lv_obj_set_style_pad_all(cell, 0, 0);
		lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

		btn = lv_button_create(cell);
		lv_obj_set_size(btn, 72, 72);
		lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 4);
		lv_obj_set_style_radius(btn, 20, 0);
		lv_obj_set_style_bg_color(btn, lv_color_hex(a->c1), 0);
		lv_obj_set_style_bg_grad_color(btn, lv_color_hex(a->c2), 0);
		lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, 0);
		lv_obj_set_style_shadow_width(btn, 18, 0);
		lv_obj_set_style_shadow_color(btn, lv_color_hex(a->c2), 0);
		lv_obj_set_style_shadow_opa(btn, LV_OPA_50, 0);
		lv_obj_set_style_transform_scale(btn, 230, LV_STATE_PRESSED);
		lv_obj_set_style_transform_pivot_x(btn, 36, 0);
		lv_obj_set_style_transform_pivot_y(btn, 36, 0);
		lv_obj_add_event_cb(btn, a->cb, LV_EVENT_CLICKED, (void *)a->arg);

		icon = lv_label_create(btn);
		lv_label_set_text(icon, a->icon);
		lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
		lv_obj_center(icon);

		name = lv_label_create(cell);
		lv_label_set_text(name, a->name);
		lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
		lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -8);
	}

	lv_screen_load(home_scr);
}

/* -------------------------------------------------------------- buttons */

static void keys_poll(lv_timer_t *t)
{
	struct input_event ev;

	LV_UNUSED(t);
	if (keys_fd < 0)
		return;
	while (read(keys_fd, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type != EV_KEY || ev.value != 1)
			continue;
		switch (ev.code) {
		case KEY_HOMEPAGE:
			if (screen_off) {
				screen_off = false;
				backlight_power(true);
			} else {
				go_home();
			}
			break;
		case KEY_POWER:
			screen_off = !screen_off;
			backlight_power(!screen_off);
			break;
		case KEY_VOLUMEUP:
			backlight_set(backlight_get() + 10);
			break;
		case KEY_VOLUMEDOWN:
			backlight_set(backlight_get() - 10);
			break;
		}
	}
}

/* ---------------------------------------------------------------- bench */

static double redraw_ms(int n)
{
	uint32_t t0;
	int i;

	lv_refr_now(NULL);
	t0 = tick_ms();
	for (i = 0; i < n; i++) {
		lv_obj_invalidate(lv_screen_active());
		lv_refr_now(NULL);
	}
	return (double)(tick_ms() - t0) / n;
}

/* Redraw the home screen, then a fill-heavy screen of plain rectangles. */
static int bench(void)
{
	lv_obj_t *scr;
	double home, rects;
	int i;

	home = redraw_ms(20);

	scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr, lv_color_hex(0x203040), 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	for (i = 0; i < 24; i++) {
		lv_obj_t *r = lv_obj_create(scr);

		lv_obj_set_size(r, 96, 72);
		lv_obj_set_pos(r, 8 + (i % 3) * 104, 8 + (i / 3) * 58);
		lv_obj_set_style_radius(r, 0, 0);
		lv_obj_set_style_border_width(r, 0, 0);
		lv_obj_set_style_bg_color(r, lv_color_hsv_to_rgb(i * 15, 70, 90), 0);
	}
	lv_screen_load(scr);
	rects = redraw_ms(20);

	printf("n72-ui bench (%s): home screen %.1f ms, 24 rectangles %.1f ms per full redraw\n",
	       mbx_gpu_stats.submits ? "GPU" : "CPU", home, rects);
	printf("  GPU submits %lu, fills %lu, flushes %lu, pool misses %lu\n",
	       mbx_gpu_stats.submits, mbx_gpu_stats.fills, mbx_gpu_stats.flushes,
	       mbx_gpu_stats.pool_misses);
	return 0;
}

/* ----------------------------------------------------------------- main */

static void restore_tty(void)
{
	if (tty_fd >= 0)
		ioctl(tty_fd, KDSETMODE, KD_TEXT);
}

static void on_signal(int sig)
{
	restore_tty();
	signal(sig, SIG_DFL);
	raise(sig);
}

int main(int argc, char **argv)
{
	const char *fb = getenv("N72_FB");
	const char *touch = getenv("N72_TOUCH");
	const char *keys = getenv("N72_KEYS");
	const char *cal = getenv("N72_TOUCH_CAL");
	const char *swap = getenv("N72_TOUCH_SWAP");
	lv_display_t *disp;
	lv_indev_t *indev;
	int i;

	for (i = 0; i < argc && i < 3; i++)
		self_argv[i] = argv[i];

	/* Keep the text console from drawing over the UI. */
	tty_fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
	if (tty_fd >= 0)
		ioctl(tty_fd, KDSETMODE, KD_GRAPHICS);
	atexit(restore_tty);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	lv_init();
	lv_tick_set_cb(tick_ms);
	backlight_init();

	disp = NULL;
	if (!getenv("N72_GPU") || strcmp(getenv("N72_GPU"), "0"))
		disp = mbx_gpu_display_create();
	if (!disp) {
		disp = lv_linux_fbdev_create();
		lv_linux_fbdev_set_file(disp, fb ? fb : "/dev/fb0");
	}
	lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
			      lv_palette_main(LV_PALETTE_PINK), true,
			      &lv_font_montserrat_14);

	if (!touch)
		touch = find_input("Zephyr2");
	if (touch) {
		indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch);
		if (indev && cal) {
			int c[4];

			if (sscanf(cal, "%d %d %d %d", &c[0], &c[1], &c[2],
				   &c[3]) == 4)
				lv_evdev_set_calibration(indev, c[0], c[1],
							 c[2], c[3]);
		}
		if (indev && swap && *swap == '1')
			lv_evdev_set_swap_axes(indev, true);
	}

	if (!keys)
		keys = find_input("gpio-keys");
	if (keys)
		keys_fd = open(keys, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	lv_timer_create(keys_poll, 30, NULL);

	build_home();
	if (argc > 1 && !strcmp(argv[1], "--bench"))
		return bench();

	for (;;) {
		uint32_t idle = lv_timer_handler();

		usleep(LV_CLAMP(1, idle, 20) * 1000);
	}
	return 0;
}

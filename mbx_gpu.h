/* SPDX-License-Identifier: MIT */
#ifndef N72_MBX_GPU_H
#define N72_MBX_GPU_H

#include "lvgl/lvgl.h"

struct mbx_gpu_stats {
	unsigned long submits, fills, flushes, pool_misses;
};
extern struct mbx_gpu_stats mbx_gpu_stats;

/*
 * Create the display on the MBX GPU (/dev/mbx): GPU-mapped draw buffers, a
 * GPU fill unit and GPU flushes. Returns NULL if the GPU is not available;
 * call it after lv_init() and before creating any other display.
 */
lv_display_t *mbx_gpu_display_create(void);

#endif

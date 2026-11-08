#!/bin/sh
# Cross-build n72-ui statically for the iPod touch 2G (ARM1176, armel).
# Run in the n72-build container: sh n72-ui/build.sh  (from the workspace root)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
UI="$ROOT/n72-ui"
LVGL="$ROOT/lvgl-src/lvgl"
OUT="$ROOT/n72-ui-build"
CC=arm-linux-gnueabi-gcc
CFLAGS="-O2 -idirafter $ROOT/linux-s5l87xx/include/uapi -mcpu=arm1176jzf-s -mfpu=vfp -mfloat-abi=softfp -ffunction-sections -fdata-sections -DLV_CONF_INCLUDE_SIMPLE -I$UI -I$ROOT/lvgl-src"
mkdir -p "$OUT/obj"
SRCS=$(find "$LVGL/src" "$LVGL/demos/widgets" "$LVGL/demos/music" -name '*.c'; echo "$LVGL/demos/lv_demos.c")
obj() { echo "$OUT/obj/$(echo "${1#$LVGL/}" | tr '/' '_' | sed 's/\.c$/.o/')"; }
OBJS=$(for f in $SRCS; do obj "$f"; done)
for f in $SRCS; do
	o=$(obj "$f")
	if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "$UI/lv_conf.h" -nt "$o" ]; then
		echo "$CC -c $f -o $o $CFLAGS"
	fi
done | xargs -P "$(nproc)" -I{} sh -c '{}'
$CC $CFLAGS -c "$UI/main.c" -o "$OUT/obj/main.o"
$CC $CFLAGS -c "$UI/mbx_gpu.c" -o "$OUT/obj/mbx_gpu.o"
$CC -static -Wl,--gc-sections -o "$OUT/n72-ui" "$OUT/obj/main.o" "$OUT/obj/mbx_gpu.o" $OBJS -lm
arm-linux-gnueabi-strip "$OUT/n72-ui"
ls -l "$OUT/n72-ui"

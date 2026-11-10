# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# Show a PNG with LVGL on the VIXEN-32 (MicroPython port, LVGL build).
#
# Copy this script and vixen.png (64x64, with transparency) to
# /vixen-32/apps/micropython/examples/ on the SD card and run it:
#
#     import png_demo             # examples/ is on sys.path
#
# Without examples/vixen.png it shows a tiny embedded 16x16 PNG instead. The image
# can be dragged with the mouse.
#
# The recipe for your own PNGs is the four lines marked "PNG" below: read the
# file's bytes, wrap them in an image descriptor, give it to an lv.image. LVGL's
# PNG decoder (lodepng) decodes it; the decoded image is cached (512 KB cache),
# so it is decoded once, not on every redraw. Keep images small: decoding takes
# memory (width * height * 4 bytes) and time on a 100 MHz CPU.

import binascii

import lvgl as lv
import vixen

PNG_FILE = '/sd/vixen-32/apps/micropython/examples/vixen.png'
FALLBACK_PNG = binascii.unhexlify('89504e470d0a1a0a0000000d49484452000000100000001008060000001ff3ff610000006d4944415478da6360c002342a9ea500f17f349cc24008e0d0489c414089d7c80a6df645a16034435ee3b4195d231e8352900dc0d0fc61811c0ac666084edbd135a31b82e20a7c9a9fd96880313e43701a00d38c6e08fd0c20c50b940522c5d1489584447152a64a6622373b0300ec62f9d83b0d202f0000000049454e44ae426082')

try:
    vixen.lv_display()
except RuntimeError:
    pass                    # already set up by an earlier program
scr = lv.screen_active()

try:
    with open(PNG_FILE, 'rb') as f:                     # PNG: read the file
        png_data = f.read()
    source = 'examples/vixen.png'
except OSError:
    png_data = FALLBACK_PNG
    source = 'built-in 16x16 PNG (no examples/vixen.png)'

# the size from the PNG's own header (IHDR: width at byte 16, height at 20)
width = int.from_bytes(png_data[16:20], 'big')
height = int.from_bytes(png_data[20:24], 'big')

png_dsc = lv.image_dsc_t({'data_size': len(png_data), 'data': png_data})   # PNG: describe it
image = lv.image(scr)                                                     # PNG: an image widget
image.set_src(png_dsc)                                                    # PNG: show it
# absolute position (top-left alignment): the drag handler adds to get_x/get_y,
# which with center() would be offsets from the middle and run away
image.set_pos((384 - width) // 2, (224 - height) // 2)

caption = lv.label(scr)
caption.set_text(source + "\n%d x %d px, drag it with the mouse" % (width, height))
caption.set_style_text_align(lv.TEXT_ALIGN.CENTER, 0)
caption.align(lv.ALIGN.TOP_MID, 0, 12)


def drag(e):
    # move the image by the pointer's movement since the last event
    vect = lv.point_t()
    lv.indev_active().get_vect(vect)
    image.set_pos(image.get_x() + vect.x, image.get_y() + vect.y)


image.add_flag(lv.obj.FLAG.CLICKABLE)
image.add_event_cb(drag, lv.EVENT.PRESSING, None)

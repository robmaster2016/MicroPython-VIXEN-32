# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# LVGL build: display driver, widget creation, rendering and a PNG decode
# (the frame lands in the emulator's chip RAM; nothing is shown). In a build
# without LVGL this test passes trivially.
try:
    import lvgl as lv
except ImportError:
    lv = None
    print("no lvgl in this build")

if lv:
    import vixen, time, gc
    vixen.lv_display()
    scr = lv.screen_active()
    lbl = lv.label(scr)
    lbl.set_text("Feel the magic")
    btn = lv.button(scr)
    btn.align(lv.ALIGN.CENTER, 0, 30)
    lv.label(btn).set_text("OK")
    bar = lv.bar(scr)
    bar.set_value(70, False)   # no animation
    chart = lv.chart(scr)
    for _ in range(20):
        lv.timer_handler()
    lv.refr_now(None)
    assert lbl.get_text() == "Feel the magic"
    assert bar.get_value() == 70
    # a tiny 1x1 PNG through lodepng
    import binascii
    png = binascii.a2b_base64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==")
    dsc = lv.image_dsc_t({"data_size": len(png), "data": png})
    img = lv.image(scr)
    img.set_src(dsc)
    lv.refr_now(None)
    assert (img.get_width(), img.get_height()) == (1, 1), (img.get_width(), img.get_height())
    gc.collect()
    print("widgets ok, mem_free", gc.mem_free())
print("TEST OK")

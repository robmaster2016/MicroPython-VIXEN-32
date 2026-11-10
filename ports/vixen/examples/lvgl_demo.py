# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# LVGL demo for the VIXEN-32 MicroPython port (LVGL build).
#
# Three tabs -- widgets with an
# animated bar and a spinner, a live line chart, an info page. Copy it to
# /vixen-32/apps/micropython/examples/ on the SD card and run it:
#
#     import lvgl_demo            # examples/ is on sys.path
#
# Mouse in port 1. TAB / shift+TAB (or cursor right/left) switch the tabs.
# F1 switches between the GUI and the REPL console. No main loop needed: the
# port runs lv_timer_handler() in the background (see vx_lvgl_mp.c).

import math
import random

import lvgl as lv
import vixen

try:
    vixen.lv_display()
except RuntimeError:
    pass                    # already set up by an earlier program
scr = lv.screen_active()

tv = lv.tabview(scr)
tv.set_tab_bar_size(22)
tab_controls = tv.add_tab("Controls")
tab_chart = tv.add_tab("Chart")
tab_info = tv.add_tab("Info")


def row(parent, align=lv.FLEX_ALIGN.START):
    # a transparent flex row spanning the tab, children vertically centred
    r = lv.obj(parent)
    r.remove_style_all()
    r.set_size(lv.pct(100), lv.SIZE_CONTENT)
    r.set_flex_flow(lv.FLEX_FLOW.ROW)
    r.set_flex_align(align, lv.FLEX_ALIGN.CENTER, lv.FLEX_ALIGN.CENTER)
    r.set_style_pad_column(10, 0)
    return r


# ---- Controls: four rows spread over the tab ------------------------------
tab_controls.set_flex_flow(lv.FLEX_FLOW.COLUMN)
tab_controls.set_flex_align(lv.FLEX_ALIGN.SPACE_EVENLY, lv.FLEX_ALIGN.START, lv.FLEX_ALIGN.START)
tab_controls.set_style_pad_hor(12, 0)

r = row(tab_controls)
button = lv.button(r)
lv.label(button).set_text(lv.SYMBOL.OK + " Click me")
clicks_label = lv.label(r)
clicks_label.set_text("Clicked 0 x")
clicks_label.set_flex_grow(1)
clicks = 0


def on_click(e):
    global clicks
    clicks += 1
    clicks_label.set_text("Clicked %d x" % clicks)


button.add_event_cb(on_click, lv.EVENT.CLICKED, None)
dropdown = lv.dropdown(r)
dropdown.set_options("MEGA65\nVIXEN-32\nC64\nAmiga")
dropdown.set_width(96)

r = row(tab_controls)
slider = lv.slider(r)
slider.set_flex_grow(1)
slider.set_value(40, False)    # False = LV_ANIM_OFF (a plain bool in LVGL 9.3)
slider_label = lv.label(r)
slider_label.set_width(36)
slider_label.set_text("40 %")
slider.add_event_cb(lambda e: slider_label.set_text("%d %%" % slider.get_value()),
                    lv.EVENT.VALUE_CHANGED, None)

r = row(tab_controls, lv.FLEX_ALIGN.SPACE_EVENLY)
group = row(r)                       # switch + its label stay together
group.set_width(lv.SIZE_CONTENT)
group.set_style_pad_column(6, 0)
dither = lv.switch(group)
dither.add_event_cb(lambda e: vixen.lv_dither(dither.has_state(lv.STATE.CHECKED)),
                    lv.EVENT.VALUE_CHANGED, None)
lv.label(group).set_text("Dither")
check = lv.checkbox(r)
check.set_text("Check")
spinner = lv.spinner(r)
spinner.set_size(28, 28)

r = row(tab_controls)
bar = lv.bar(r)
bar.set_height(8)
bar.set_flex_grow(1)
anim = lv.anim_t()
anim.init()
anim.set_var(bar)
anim.set_values(0, 100)
anim.set_duration(2000)
anim.set_reverse_duration(2000)
anim.set_repeat_count(lv.ANIM_REPEAT_INFINITE)
anim.set_custom_exec_cb(lambda a, v: bar.set_value(v, False))
anim.start()

# ---- Chart: two series, a new point every 100 ms ---------------------------
chart = lv.chart(tab_chart)
chart.set_size(lv.pct(100), lv.pct(100))
chart.set_type(lv.chart.TYPE.LINE)
chart.set_point_count(40)
series_sin = chart.add_series(lv.palette_main(lv.PALETTE.CYAN), lv.chart.AXIS.PRIMARY_Y)
series_rnd = chart.add_series(lv.palette_main(lv.PALETTE.ORANGE), lv.chart.AXIS.PRIMARY_Y)
phase = 0


def chart_tick(timer):
    global phase
    phase += 1
    chart.set_next_value(series_sin, int(50 + 40 * math.sin(phase * 0.12)))
    chart.set_next_value(series_rnd, random.randint(10, 90))


chart_timer = lv.timer_create(chart_tick, 100, None)

# ---- Info --------------------------------------------------------------------
info = lv.label(tab_info)
info.set_text("MicroPython + LVGL on the VIXEN-32\n\n"
              "Mouse: port 1\n"
              "TAB / shift+TAB: next / previous tab\n"
              "F1: GUI <-> REPL console\n"
              "MEGA/ALT + : ; pound - @  ->  { } | ~ `")


# ---- Keyboard: TAB / shift+TAB (and cursor right/left) switch the tabs ------
# vixen.lv_keys() gets the keys while the GUI is shown (F1 = console).
def on_key(key):
    count = tv.get_tab_count()
    active = tv.get_tab_active()
    if key in (lv.KEY.NEXT, lv.KEY.RIGHT):
        tv.set_active((active + 1) % count, False)
    elif key in (lv.KEY.PREV, lv.KEY.LEFT):
        tv.set_active((active - 1) % count, False)


vixen.lv_keys(on_key)

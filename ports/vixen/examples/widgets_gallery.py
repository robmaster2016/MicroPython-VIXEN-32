# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# Widget gallery for the VIXEN-32 MicroPython port (LVGL build) -- demo and test.
#
# Creates every LVGL widget the build contains, each with a small typical set-up,
# on four tabs (scroll a tab by dragging it with the mouse). At the end it prints
# which widgets worked and which raised, so it doubles as a smoke test:
#
#     import widgets_gallery      # examples/ is on sys.path
#
# TAB / shift+TAB switch the tabs; F1 switches between the GUI and the
# console with the report.

import lvgl as lv
import vixen

try:
    vixen.lv_display()
except RuntimeError:
    pass                    # already set up by an earlier program
ok = []
failed = []


def card(parent, name, build):
    # a small titled box; build(box) creates the widget inside it
    box = lv.obj(parent)
    box.set_size(lv.SIZE_CONTENT, lv.SIZE_CONTENT)
    box.set_style_pad_all(4, 0)
    box.set_flex_flow(lv.FLEX_FLOW.COLUMN)
    box.set_flex_align(lv.FLEX_ALIGN.START, lv.FLEX_ALIGN.CENTER, lv.FLEX_ALIGN.CENTER)
    box.set_style_pad_row(3, 0)
    title = lv.label(box)
    title.set_text(name)
    title.set_style_text_color(lv.palette_lighten(lv.PALETTE.GREY, 2), 0)
    try:
        build(box)
        ok.append(name)
    except Exception as e:
        failed.append("%s: %r" % (name, e))
        lv.label(box).set_text("ERROR")


def page(tv, name):
    tab = tv.add_tab(name)
    tab.set_flex_flow(lv.FLEX_FLOW.ROW_WRAP)
    tab.set_style_pad_all(6, 0)
    tab.set_style_pad_gap(6, 0)
    return tab


tv = lv.tabview(lv.screen_active())
tv.set_tab_bar_size(22)
basic = page(tv, "Basic")
inputs = page(tv, "Input")
data = page(tv, "Data")
more = page(tv, "More")


# ---- Basic -------------------------------------------------------------------
def w_label(p):
    lv.label(p).set_text("Text " + lv.SYMBOL.OK)


def w_button(p):
    b = lv.button(p)
    lv.label(b).set_text("Button")


def w_checkbox(p):
    lv.checkbox(p).set_text("Check")


def w_switch(p):
    lv.switch(p).add_state(lv.STATE.CHECKED)


def w_slider(p):
    s = lv.slider(p)
    s.set_width(100)
    s.set_value(60, False)


def w_bar(p):
    b = lv.bar(p)
    b.set_size(100, 8)
    b.set_value(40, False)


def w_arc(p):
    a = lv.arc(p)
    a.set_size(56, 56)
    a.set_value(70)


def w_spinner(p):
    lv.spinner(p).set_size(32, 32)


def w_led(p):
    led = lv.led(p)
    led.set_size(16, 16)
    led.set_color(lv.palette_main(lv.PALETTE.GREEN))
    led.on()


def w_line(p):
    pts = [lv.point_precise_t({'x': 0, 'y': 20}), lv.point_precise_t({'x': 30, 'y': 0}),
           lv.point_precise_t({'x': 60, 'y': 20}), lv.point_precise_t({'x': 90, 'y': 0})]
    line = lv.line(p)
    line.set_points(pts, len(pts))
    line.set_style_line_width(2, 0)


def w_image(p):
    lv.image(p).set_src(lv.SYMBOL.IMAGE + " symbol")


for name, fn in (("label", w_label), ("button", w_button), ("checkbox", w_checkbox),
                 ("switch", w_switch), ("slider", w_slider), ("bar", w_bar), ("arc", w_arc),
                 ("spinner", w_spinner), ("led", w_led), ("line", w_line), ("image", w_image)):
    card(basic, name, fn)


# ---- Input -------------------------------------------------------------------
def w_dropdown(p):
    d = lv.dropdown(p)
    d.set_options("MEGA65\nVIXEN-32\nC64")
    d.set_width(96)


def w_roller(p):
    r = lv.roller(p)
    r.set_options("Mon\nTue\nWed\nThu\nFri", lv.roller.MODE.NORMAL)
    r.set_visible_row_count(3)


def w_spinbox(p):
    s = lv.spinbox(p)
    s.set_range(-9999, 9999)
    s.set_digit_format(4, 2)
    s.set_value(1234)
    s.set_width(70)


def w_textarea(p):
    t = lv.textarea(p)
    t.set_size(120, 36)
    t.set_text("type here")


def w_buttonmatrix(p):
    m = lv.buttonmatrix(p)
    m.set_map(["1", "2", "3", "\n", "4", "5", "6", ""])
    m.set_size(110, 60)


def w_keyboard(p):
    t = lv.textarea(p)
    t.set_size(200, 30)
    k = lv.keyboard(p)
    k.set_size(340, 110)
    k.set_textarea(t)


for name, fn in (("dropdown", w_dropdown), ("roller", w_roller), ("spinbox", w_spinbox),
                 ("textarea", w_textarea), ("buttonmatrix", w_buttonmatrix), ("keyboard", w_keyboard)):
    card(inputs, name, fn)


# ---- Data --------------------------------------------------------------------
def w_chart(p):
    c = lv.chart(p)
    c.set_size(140, 70)
    s = c.add_series(lv.palette_main(lv.PALETTE.CYAN), lv.chart.AXIS.PRIMARY_Y)
    for v in (10, 40, 30, 70, 50, 90, 60):
        c.set_next_value(s, v)


def w_table(p):
    t = lv.table(p)
    for r, row in enumerate((("Chip", "MHz"), ("VIXEN", "100"), ("6502", "1"))):
        for c, text in enumerate(row):
            t.set_cell_value(r, c, text)
    t.set_column_width(0, 60)
    t.set_column_width(1, 50)


def w_scale(p):
    s = lv.scale(p)
    s.set_size(140, 30)
    s.set_range(0, 100)
    s.set_total_tick_count(11)
    s.set_major_tick_every(5)


def w_calendar(p):
    c = lv.calendar(p)
    c.set_size(180, 150)
    c.set_today_date(2026, 10, 30)
    c.set_month_shown(2026, 10)


def w_spangroup(p):
    sg = lv.spangroup(p)
    sg.set_width(140)
    a = sg.add_span()
    a.set_text("Rich ")
    b = sg.add_span()
    b.set_text("text")
    b.style.set_text_color(lv.palette_main(lv.PALETTE.ORANGE))


for name, fn in (("chart", w_chart), ("table", w_table), ("scale", w_scale),
                 ("spangroup", w_spangroup), ("calendar", w_calendar)):
    card(data, name, fn)


# ---- More --------------------------------------------------------------------
def w_list(p):
    lst = lv.list(p)
    lst.set_size(120, 90)
    lst.add_text("Files")
    lst.add_button(lv.SYMBOL.FILE, "boot.py")
    lst.add_button(lv.SYMBOL.FILE, "main.py")


def w_menu(p):
    m = lv.menu(p)
    m.set_size(150, 80)
    main_page = lv.menu_page(m, None)
    cont = lv.menu_cont(main_page)
    lv.label(cont).set_text("Menu item")
    m.set_page(main_page)


def w_msgbox(p):
    mb = lv.msgbox(p)
    mb.add_title("Hello")
    mb.add_text("Message box")
    mb.add_close_button()


def w_win(p):
    w = lv.win(p)
    w.set_size(140, 80)
    w.add_title("Window")
    lv.label(w.get_content()).set_text("content")


def w_tileview(p):
    t = lv.tileview(p)
    t.set_size(120, 60)
    lv.label(t.add_tile(0, 0, lv.DIR.RIGHT)).set_text("Tile 1 >")
    lv.label(t.add_tile(1, 0, lv.DIR.LEFT)).set_text("< Tile 2")


def w_canvas(p):
    buf = lv.draw_buf_create(48, 32, lv.COLOR_FORMAT.RGB565, 0)
    c = lv.canvas(p)
    c.set_draw_buf(buf)
    c.fill_bg(lv.palette_main(lv.PALETTE.PURPLE), lv.OPA.COVER)


def w_imagebutton(p):
    ib = lv.imagebutton(p)
    ib.set_size(40, 20)


def w_animimg(p):
    lv.animimg(p).set_size(20, 20)


for name, fn in (("list", w_list), ("menu", w_menu), ("msgbox", w_msgbox), ("win", w_win),
                 ("tileview", w_tileview), ("canvas", w_canvas), ("imagebutton", w_imagebutton),
                 ("animimg", w_animimg)):
    card(more, name, fn)


# ---- Keyboard: TAB / shift+TAB (and cursor right/left) switch the tabs ------
def on_key(key):
    count = tv.get_tab_count()
    active = tv.get_tab_active()
    if key in (lv.KEY.NEXT, lv.KEY.RIGHT):
        tv.set_active((active + 1) % count, False)
    elif key in (lv.KEY.PREV, lv.KEY.LEFT):
        tv.set_active((active - 1) % count, False)


vixen.lv_keys(on_key)

print("widgets OK (%d): %s" % (len(ok), " ".join(ok)))
print("widgets FAILED (%d)" % len(failed))
for f in failed:
    print("  " + f)

# LVGL graphics

The LVGL build of the VIXEN-32 MicroPython port includes the `lvgl` module
(LVGL 9.3 via [lv_binding_micropython](https://github.com/lvgl/lv_binding_micropython))
and a display, mouse and keyboard driver for the VIXEN hardware.

## Firmware

Build the LVGL firmware from the repository root (see [Building](building.md)):

```sh
./build.sh LVGL=1 BUILD=build-lvgl-net       # LVGL + network + sound
./build.sh LVGL=1 NET=0 BUILD=build-lvgl     # LVGL + sound, no network
```

The result is `ports/vixen/<BUILD>/firmware.rvk`. Copy it to the SD card and
load it from the VIXEN-32 **F12/Help -> Load RVK** menu. The build without
LVGL (`./build.sh`) is smaller and does not contain the `lvgl` module.

## Quick start

```python
import lvgl as lv, vixen        # importing lvgl initialises LVGL (no lv.init())
vixen.lv_display()              # 384x224 screen + mouse; hides the console
b = lv.button(lv.screen_active())
lv.label(b).set_text("Hello VIXEN")
b.center()
```

No main loop is needed: LVGL keeps running in the background, also while the
REPL waits for input or `time.sleep()` runs.

## Folder on the SD card

Everything MicroPython uses lives in one folder on the card:

```
/vixen-32/apps/micropython/          (in MicroPython: /sd/vixen-32/apps/micropython)
├── boot/
│   ├── boot.py                      autostart, part 1 (optional)
│   └── main.py                      autostart, part 2 (optional)
├── examples/                        examples (lvgl_demo.py, vixen.png, ...)
└── lib/                             your own modules (optional)
```

At startup this folder is the current directory, so relative paths such as
`open('examples/vixen.png')` work directly. `examples/` and `lib/` are on the
module search path (`sys.path`): run an example or load a module with
`import name` (without `.py`). Importing the same module a second time does
nothing; to run it again, press Ctrl+D (soft reset) and `import` it again.

## Autostart

At power-on and after every soft reset (Ctrl+D), MicroPython runs these files
from `/vixen-32/apps/micropython` if they exist:

1. `boot/boot.py`
2. `boot/main.py`

**RUN/STOP** or **Ctrl+C** interrupts a running `main.py` and drops to the
REPL. A GUI built by `main.py` stays on screen and keeps responding.

Example `boot/main.py` that starts the demo at power-on:

```python
import lvgl_demo
```

## Keyboard

| Key | Action |
|---|---|
| **F1** | switch between GUI and REPL console |
| **TAB** / Shift+TAB | in `lvgl_demo` and `widgets_gallery`: next / previous tab (cursor right/left too) |
| RUN/STOP, Ctrl+C | interrupt the running program |
| Ctrl+D | soft reset (afterwards `import lvgl` and `vixen.lv_display()` again) |
| MEGA or ALT + `:` `;` `£` `-` `@` | `{` `}` `\|` `~` `` ` `` (missing on the MEGA65 keyboard) |

By default the keyboard goes to the REPL, even while the GUI is visible (you
then type "blind"). A program can claim the keys for itself:

```python
def on_key(key):                 # called for every key press
    if key == lv.KEY.NEXT:       # TAB
        ...
vixen.lv_keys(on_key)            # from now on keys go to on_key while the GUI is visible
vixen.lv_keys(None)              # everything back to the REPL
```

While the GUI is visible and a handler is set, the handler receives **every**
key instead of the REPL. To type in the REPL, press **F1** first. Ctrl+C and
RUN/STOP always go to MicroPython.

| Key | `key` |
|---|---|
| TAB / Shift+TAB | `lv.KEY.NEXT` / `lv.KEY.PREV` |
| Cursor keys | `lv.KEY.UP`, `DOWN`, `LEFT`, `RIGHT` |
| RETURN | `lv.KEY.ENTER` |
| others | ASCII code, e.g. `ord('a')` |

With this you can feed keyboard input into LVGL text areas yourself
(`textarea.add_char(key)`). The on-screen keyboard `lv.keyboard` works with the
mouse out of the box.

## Mouse

The VIXEN core reads an **Amiga mouse in joystick port 1** directly from the
pins (quadrature signals). This means:

* A **PS/2 mouse with a simple plug adapter does not work**: PS/2 is a serial
  protocol and a passive adapter translates nothing. You need an **active
  PS/2-to-Amiga converter** (a small microcontroller adapter) or a real Amiga
  mouse.
* The MEGA65 BIOS setting "Amiga mouse" applies to the MEGA65 core; the VIXEN
  core evaluates the pins itself. Trying it does no harm.
* **Check with `mouse_test.py`**: it shows counters, buttons and the raw pin
  levels. If the counters change when you move the mouse, it works. If only the
  pins change, the signal is wrong (e.g. an Atari ST mouse). If nothing changes,
  nothing arrives at the port.

The mouse pointer is a hardware sprite, so moving the mouse redraws nothing.

## Available widgets

All 30 widgets below are exercised by `widgets_gallery.py`. They are always
created with `lv.<name>(parent)`.

| Python | Widget | Typical calls |
|---|---|---|
| `lv.label` | text | `set_text("...")` |
| `lv.button` | button | `add_event_cb(cb, lv.EVENT.CLICKED, None)`, text via `lv.label(button)` |
| `lv.checkbox` | check box | `set_text("...")`, `has_state(lv.STATE.CHECKED)` |
| `lv.switch` | switch | `add_state(lv.STATE.CHECKED)` |
| `lv.slider` | slider | `set_value(40, False)`, `get_value()`, event `VALUE_CHANGED` |
| `lv.bar` | progress bar | `set_value(40, False)` |
| `lv.arc` | arc / knob | `set_value(70)`, `set_size(56, 56)` |
| `lv.spinner` | loading animation | `set_size(32, 32)` |
| `lv.led` | LED | `set_color(...)`, `on()`, `off()` |
| `lv.line` | polyline | `set_points([lv.point_precise_t({'x':0,'y':0}), ...], n)` |
| `lv.image` | image (PNG, symbol) | `set_src(dsc)`, see [Showing a PNG](#showing-a-png) |
| `lv.imagebutton` | image button | `set_src(...)` per state |
| `lv.animimg` | image animation | `set_src([...])`, `start()` |
| `lv.dropdown` | drop-down list | `set_options("A\nB\nC")`, `get_selected()` |
| `lv.roller` | roller | `set_options("A\nB", lv.roller.MODE.NORMAL)` |
| `lv.spinbox` | number input | `set_range(-9999, 9999)`, `set_digit_format(4, 2)` |
| `lv.textarea` | text area | `set_text("...")`, `set_one_line(True)` |
| `lv.keyboard` | on-screen keyboard | `set_textarea(textarea)` |
| `lv.buttonmatrix` | button matrix | `set_map(["1","2","\n","3",""])` |
| `lv.chart` | chart | `add_series(color, lv.chart.AXIS.PRIMARY_Y)`, `set_next_value(s, v)` |
| `lv.table` | table | `set_cell_value(row, col, "...")` |
| `lv.scale` | scale | `set_range(0, 100)`, `set_total_tick_count(11)` |
| `lv.calendar` | calendar | `set_today_date(2026, 10, 30)`, `set_month_shown(2026, 10)` |
| `lv.spangroup` | formatted text | `add_span().set_text("...")` |
| `lv.list` | list | `add_text("...")`, `add_button(lv.SYMBOL.FILE, "...")` |
| `lv.menu` | menu | `lv.menu_page(menu, None)`, `set_page(page)` |
| `lv.msgbox` | message box | `add_title`, `add_text`, `add_close_button()` |
| `lv.win` | window | `add_title("...")`, `get_content()` |
| `lv.tabview` | tabs | `add_tab("...")`, `set_tab_bar_size(22)` |
| `lv.tileview` | tile pages | `add_tile(0, 0, lv.DIR.RIGHT)` |
| `lv.canvas` | drawing canvas | `set_draw_buf(lv.draw_buf_create(w, h, lv.COLOR_FORMAT.RGB565, 0))` |

Also available: the base object `lv.obj` (containers, layout with
`set_flex_flow`, `set_flex_align`, `set_grid_...`), styles, animations
(`lv.anim_t`), timers (`lv.timer_create(cb, ms, None)`) and symbols
(`lv.SYMBOL.OK`, `...FILE`, `...IMAGE`, ...).

**Not included:** GIF, JPEG, QR code, barcode, Lottie/vector graphics,
FreeType. Fonts: Montserrat 10 (default), 12 and 14.

LVGL 9.3 notes: `LV_ANIM_OFF`/`LV_ANIM_ON` are plain `False`/`True`
(`slider.set_value(40, False)`), and there is no `lv.init()`.

## Showing a PNG

```python
d = open('examples/vixen.png', 'rb').read()                  # read the file
dsc = lv.image_dsc_t({'data_size': len(d), 'data': d})        # describe it
img = lv.image(lv.screen_active())                            # image widget
img.set_src(dsc)                                              # show it
```

* Ready-made example: copy `png_demo.py` and `vixen.png` (64x64 with
  transparency) to `examples/`, then `import png_demo`. Without `vixen.png` it
  shows a built-in 16x16 image. The image can be dragged with the mouse.
* Transparency (alpha) works.
* Keep images small: decoding needs width x height x 4 bytes of memory and takes
  noticeable time at 100 MHz. Decoded images are cached (512 KB), so each is
  decoded only once.
* To move an image, use `set_pos(x, y)` instead of `center()` (with centred
  alignment, x/y are offsets from the centre).

## Colours

The display has 256 fixed colours (a 6x6x6 colour cube plus 40 grey levels).
LVGL renders in RGB565 internally, so gradients can show banding;
`vixen.lv_dither(True)` enables 4x4 dithering.

## Limits

* The LVGL firmware keeps its C stack (16 KB) in fast BRAM. Python recursion
  goes about 48 levels deep before `RuntimeError: maximum recursion depth
  exceeded`.

## Examples

The examples are in `ports/vixen/examples/` in this repository.

| File | Purpose |
|---|---|
| `lvgl_demo.py` | tabs, widgets, animated bar, live chart; TAB switches tabs |
| `widgets_gallery.py` | every widget once, plus a report in the console (F1); TAB switches tabs |
| `png_demo.py` + `vixen.png` | load a PNG, show it, drag it with the mouse |
| `mouse_test.py` | mouse diagnostics (counters, buttons, pin levels); also works without LVGL |

To run them, copy the files to `/vixen-32/apps/micropython/examples/` on the
card, then e.g. `import widgets_gallery`.

## Quick check

1. `import mouse_test` and move the mouse (see [Mouse](#mouse)).
2. `import widgets_gallery`, press F1: the report should list 30 OK.
3. `import png_demo` with `vixen.png` in `examples/`.
4. Put `print('Autostart')` in `boot/main.py`: it should appear at startup.
5. `import lvgl_demo`, then TAB / Shift+TAB should switch tabs.
6. Ctrl+C interrupts `while True: pass`; F1 switches views; Ctrl+D restarts.

The same functions are covered in the emulator test suite; see
[Emulator](emulator.md) and [Testing](testing.md).

# Screenshots

Real firmware (`build-lvgl-net`), native VIXEN-32 resolution 384 x 224,
taken in the [emulator](../emulator.md) with `make_screenshots.sh`. The
`@2x` files are the same pictures scaled 2x with nearest neighbour.

`gfx_demo` is the one picture the emulator cannot draw (it does not model
the blitter's triangles): the MEGA65 drew it, `gfx_demo_capture.py` saved
the framebuffer, and `make_screenshots.sh` replays `gfx_demo.fb.z` in the
emulator so it comes out like the others.

`console92` and `sqlite` show the hi-res console (92 columns, the default):
768 x 224 dots that are half as wide as they are high, so their `@2x` copy
doubles the height only (768 x 448).

Text screens use the VIXEN-32 ROM font, the MEGA65 ASCII font by deft (used
in the VIXEN-32 core with the author's permission). Weather and clock show
live data from the time they were taken. Not shown: the mouse pointer (a
hardware sprite).

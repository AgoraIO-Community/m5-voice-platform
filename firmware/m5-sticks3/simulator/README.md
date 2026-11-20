# M5Stack voice UI simulator

These deterministic native executables render the production `main/voice_ui.cpp`, image,
fonts and `lv_conf.h` with LVGL 8.4.0 into the selected board's RGB565 framebuffer. They use
software flush and needs no window server, SDL, network service, credentials or hardware.

From this project root:

```bash
cmake -S simulator -B simulator/build -DCMAKE_BUILD_TYPE=Release
cmake --build simulator/build -j 8
simulator/build/voice_ui_sim simulator/captures
python3 simulator/render_captures.py simulator/captures
```

The board-specific smoke executable uses the same renderer and checks the two
profiles used by the workshop:

```bash
simulator/build/voice_ui_multiboard_smoke sticks3 simulator/captures/sticks3
simulator/build/voice_ui_multiboard_smoke stopwatch simulator/captures/stopwatch
```

It renders the 135×240 StickS3 and 468×468 Stopwatch layouts and verifies
that every visible LVGL object remains inside its display.
The Stopwatch profile keeps every label inside the active circular OLED area.

The project uses `components/lvgl` when installed. Otherwise CMake fetches
LVGL tag `v8.4.0`; an existing matching checkout can be passed explicitly with
`-DLVGL_SOURCE_DIR=/absolute/path/to/lvgl`. CMake, a C/C++17 compiler and Python Pillow
(only for PNG/GIF conversion) are required.

The executable runs all fixtures and exits: idle, A pressed, connecting, 15/3/0 seconds
remaining, listening, thinking, speaking, ended, error, offline, missing/low/full battery,
charging and all six volume levels. Sixteen application diagnostics exercise the generated CJK subset. There
are no keyboard/window controls; press Ctrl+C to stop or rerun to reset. The only argument
selects the output directory. Button behavior is injected as `button_pressed`; the actual
M5Unified A button and session start/stop behavior belong to firmware validation.

The face is two LVGL-drawn eyes (rounded rectangles with background-colored lids), not
images. Idle blinks, double-blinks and glances on a deterministic pseudo-random schedule;
Connecting scans left and right; Ready/Listening open wider and Listening grows with the
mic level; Thinking looks up to the side with one eye squinting; Speaking shows smiling
arches that bounce with playback level; Ended shows closed arches; Error shows lowered
lids; Offline shows two thin lines. Ended/Error/Offline settle and stop redrawing.
Volume appears as a right-side vertical track on rectangular boards and a short
right-side arc on Stopwatch; both fill through the six 0/20/40/60/80/100% levels. An empty
muted-red track indicates 0%. The Stopwatch arc sits between about 2 o'clock and 4 o'clock
and fills upward.

The captures folder contains native PPM/PNG files, `expression-preview.png` (four key states), `states-contact-sheet.png` at 2× nearest
neighbor scale, and `motion-idle.gif` / `motion-connect.gif` / `motion-speak.gif` at 3×. These files come from
the actual LVGL framebuffer. The GIFs show the eye animation. The converter never redraws the UI in another renderer.

The executable checks on-screen object bounds, label height, fixed title/action/numeric
widths, visible glyph coverage, nonblank frames, distinct settled eye expressions, audio-driven eyes,
all six bottom-aligned volume fills, independence of volume and audio/state visuals,
idle eye motion, and no redraw on settled ended/error/offline screens. The detail line intentionally
ellipsizes long diagnostics. Repeat runs can be compared byte-for-byte for determinism.

Native build and all fixtures PASS with the eye UI; the contact sheet and both board
layouts were visually inspected. The LVGL pool finishes at about 9.3 KiB used including
allocator overhead, leaving about 6.7 KiB of the 16 KiB pool free. The eyes are six plain
LVGL objects and use no image assets; the old mascot images (`voice_ui_assets.c`,
`voice_ui_mascot_states.c`) are no longer compiled.

The latest physical screen has not yet been visually checked. These results prove layout
and UI state rendering, not panel colors, SPI throughput, physical
buttons, audio timing or firmware heap behavior.

# M5Stack voice UI implementation

The implementation targets StickS3/K150 at 135 × 240 portrait and StopWatch at
468 × 468 round, both RGB565, using ESP-IDF 6.0.3, M5Unified/M5GFX and LVGL 8.4.0. The firmware
owns M5 initialization and button input. `voice_ui_init()` owns LVGL and its synchronous
M5GFX flush; `voice_ui_update()` and `voice_ui_tick()` run only in the application UI task.
No network/audio callback may mutate LVGL. Hardware identity and audio pins are handled
by the main firmware integration, outside this UI module.

Visual direction: a friendly blue Agora voice companion with two animated eyes, one clear
Chinese conversation state and a fixed A-button action. This is a dedicated portrait
scene based on the minimal/blank LVGL template ownership pattern. The user supplied the
blue brand direction, so there are no unrelated visual alternatives.

| Token | Value |
| --- | --- |
| Background / surface | `#02070F` / `#112C46` |
| Primary / secondary text | `#F2F7FC` / `#94AEC6` |
| Agora blue / eye highlight | `#1EAEFF` / `#8FE3FF` |
| Listening / thinking / error | `#67E8C3` / `#FFD07E` / `#FF8C91` |
| Outer safe inset | 8 px |
| State / detail font | Noto Sans CJK SC Medium 18 px / 12 px |
| Header / metadata | Montserrat 14 / 12 / 10 px |
| Action radius | 7 px |

State labels cover 待机、连接中、请说话、聆听中、思考中、说话中、已结束、连接异常、网络离线.
State text accompanies every semantic color. Top-right Wi-Fi uses a Wi-Fi symbol or an
explicit cross; unavailable battery shows `--`, charging prepends `+`. The primary action
states its effect: start, cancel connection, end conversation, retry, or start again.
The side B button cycles speaker volume through 100%, 80%, 60%, 40%, 20%, 0%, then
returns to 100%. A narrow vertical track at the right of the mascot fills from the
bottom to indicate the selected volume; 0% leaves an empty muted-red track. There is
no volume percentage in the header. This control is independent of the live audio meter.

The snapshot carries the real state and remaining timeout. The UI never closes a session
or infers that a service is ready. `Ready` alone shows the 15-second countdown; values
clamp to the configured timeout, turn amber for the last three seconds, and disappear
as soon as another state arrives. Audio metering uses the supplied 0–100 level; it does
not fabricate a waveform for quiet audio.

| Trigger | Motion / feedback | Cadence and interruption |
| --- | --- | --- |
| Physical A pressed | Action surface brightens | Next snapshot; release restores |
| Connecting / thinking | Small running status marker | 180 ms phase, checked every 90 ms; state change cancels immediately |
| Mic / speaker level | Seven centered meter bars | Direct bounded level update; zero restores baseline |
| Waiting timeout | Small progress bar and whole seconds | Snapshot cadence; state change hides both |
| Conversation state changes | Ease the eye poses and lids into the new expression | 20 ms animation frames; Ended/Error/Offline settle |
| Idle | Blink, double-blink and glance around | Deterministic 20 ms updates; pixel changes only |
| Physical B pressed | Right-side volume track changes height | Next snapshot; 0% hides its fill |

The face is built from six plain LVGL objects: two eye bodies and four lids. No image
decoder or runtime image asset is linked. State and audio level drive eye width, height,
gaze and lid coverage; integer pixel geometry is cached so unchanged settled frames do
not redraw. There are no per-frame allocations, whole-screen opacity transitions, image
transforms, video decoders or dynamic widget creation after init.

Memory: StickS3 uses a 24-line RGB565 transfer buffer and StopWatch uses 12 lines. All
transfer buffers allocate internal RAM. Flushing is synchronous and makes no PSRAM/DMA
assumption. On ESP-IDF, LVGL's fixed 16 KiB pool is allocated in PSRAM when available,
with an internal RAM fallback. `voice_ui_init()` validates the allocation before
`lv_init()` and returns false on exhaustion. A C allocator hook exposes the preallocated
lifetime-long pool to LVGL; it never allocates itself. Generated fonts compile to about
11.9 KiB (12 px) and 20.6 KiB (18 px) on the native compiler; embedded ELF sizes may
differ.

Font source: Noto Sans CJK SC Medium, OFL-1.1; copyright/license retained in
`NOTO-FONT-LICENSE.txt`. `make_lvgl_font.py` uses pinned `lv_font_conv@1.5.2`; the font
manifest records the source hash and exact rebuild command. It subsets all literals from
`voice_ui.cpp` and `main.cpp` plus ASCII, currently 173 characters with no missing glyphs.
Rebuild when application text changes. LVGL's Montserrat fonts supply Wi-Fi/cross icons.

Validation: native build PASS, visual/state/glyph/layout/eye-expression/settled-redraw
PASS. The simulator exercises 16 current application diagnostics, all six volume levels,
audio-driven eye motion, idle blinking/glancing, and settled Ended/Error/Offline scenes. The latest physical screen has not yet been visually checked;
native captures do not establish panel appearance, physical input or hardware performance.
See `../simulator/README.md` for build/run/capture instructions.

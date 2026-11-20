# M5StickS3 firmware setup on macOS

The firmware in `firmware/m5-sticks3` builds for ESP32-S3 with ESP-IDF **v6.0.3**. Run `cd firmware/m5-sticks3` from the `m5-voice-platform` repository root. The commands below run from that firmware directory. [Workshop reference](../firmware/m5-sticks3/UPSTREAM.md)

## Install and activate the toolchain

Check `xcode-select -p`. If the Xcode Command Line Tools are missing, run `xcode-select --install`. With Homebrew available, install the missing formulae from the following set. ESP-IDF requires CMake 3.22 or later and Python 3.10 or later. [Espressif macOS toolchain setup](https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32s3/get-started/linux-macos-setup-legacy.html)

```sh
brew install cmake ninja ccache dfu-util python@3.12
```

Select Homebrew Python 3.12 and clone the pinned SDK with its submodules. Run the clone and installer once for a fresh setup. [Homebrew Python 3.12](https://formulae.brew.sh/formula/python@3.12) · [ESP-IDF v6.0.3 release](https://github.com/espressif/esp-idf/releases/tag/v6.0.3)

```sh
export PATH="$(brew --prefix python@3.12)/libexec/bin:$PATH"
python3 --version
git clone --recursive --branch v6.0.3 https://github.com/espressif/esp-idf.git esp-idf
export IDF_TOOLS_PATH="$PWD/.cache/espressif"
./esp-idf/install.sh esp32s3
```

In every new terminal, return to the firmware directory and activate the same SDK and tools directory. Expect `idf.py --version` to report `ESP-IDF v6.0.3`.

```sh
export PATH="$(brew --prefix python@3.12)/libexec/bin:$PATH"
export IDF_TOOLS_PATH="$PWD/.cache/espressif"
. ./esp-idf/export.sh
idf.py --version
```

Use `python3` for project scripts. ESP-IDF's installer creates its own virtual environment in the project-local `.cache/espressif/python_env/` before installing its Python packages. For other Python packages, first create a project-local `.venv` with `python3 -m venv .venv` and use `.venv/bin/python3 -m pip`. The dependency bootstrap uses the standard library. [ESP-IDF tools and virtual environments](https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32s3/api-guides/tools/idf-tools.html)

## Restore components and build

```sh
python3 tools/bootstrap_dependencies.py
python3 tools/bootstrap_dependencies.py --check
idf.py set-target esp32s3
idf.py build
```

Run `set-target` on the first build. For subsequent builds, run `idf.py build`. Follow changes to `sdkconfig.defaults` with `idf.py reconfigure` and `idf.py build`. The application image is `build/m5stack_agora.bin`.

- Bootstrap restores the bundled **M5Unified 0.2.23**, **M5GFX 0.2.30**, and **LVGL 8.4.0** snapshots offline and checks their SHA-256 hashes. Keep the bundled snapshots, which include local fixes. On a fresh build, ESP-IDF's component manager can fetch the separately locked `espressif/cjson` 1.7.19 from the registry. [Snapshot details](../firmware/m5-sticks3/vendor/README.md) · [Component lock](../firmware/m5-sticks3/dependencies.lock)
- Keep the workshop partition table and `sdkconfig.defaults`, including the 240 MHz CPU, performance settings, PSRAM, and USB Serial/JTAG console. Leave `M5GFX_BOARD` unset so the firmware detects the StickS3 or Stopwatch at boot. Verify the display, audio, input, power, and board profile before adapting the project to another board. [Firmware instructions](../firmware/m5-sticks3/AGENTS.md) · [Workshop reference](../firmware/m5-sticks3/UPSTREAM.md)

## Configure a live session and flash

A build can use the fallback settings. For a live session, create the ignored local header from the example if it does not already exist.

```sh
test -e main/voice_config_local.h || cp main/voice_config.local.example.h main/voice_config_local.h
```

Set `VOICE_WIFI_SSID` and `VOICE_WIFI_PASSWORD` for a reachable 2.4 GHz Wi-Fi network. Set `VOICE_SERVER_URL` to the backend's LAN address, for example `http://192.168.1.20:8001`. The Mac must accept connections on that LAN address and port. Match `VOICE_AREA_CODE` to the backend region. Use `0xFFFFFFFFU` for the Agora Global setup and `0x00000001U` for the Agora China setup. The example header sets China, while the firmware fallback is global. Keep Wi-Fi credentials in the local header. Keep Agora App Certificates and provider keys in the backend's ignored environment file and keep tokens out of logs.

Rebuild after editing the local header. Connect the StickS3 with a USB data cable, check that `/dev/cu.usbmodem1101` is the board's port, and close any process using it. If the path changes after reconnecting, use `ls /dev/cu.*` to identify the current callout device.

```sh
idf.py build
test -c /dev/cu.usbmodem1101
idf.py -p /dev/cu.usbmodem1101 flash
idf.py -p /dev/cu.usbmodem1101 monitor
```

Look for `BOARD=M5StickS3`, `panel=135x240`, and Wi-Fi connection status. Exit the monitor with `Ctrl+]`.

## Host checks

The transport regression runs with the host C compiler and requires no board, network, or credentials.

```sh
bash tests/transport/run.sh
```

The audio regression covers PCM16, G.722, and PCMA at both supported packet durations.

```sh
bash tests/audio/run.sh
```

After bootstrap and CMake setup, run the native [LVGL simulator and board smoke tests](../firmware/m5-sticks3/simulator/README.md). For optional PNG and GIF output, run `python3 -m venv .venv`, `.venv/bin/python3 -m pip install Pillow`, and `.venv/bin/python3 simulator/render_captures.py simulator/captures`.

### Mac readiness check (2026-11-19)

| Check | Result |
| --- | --- |
| Host tools | Homebrew, Git, Apple Clang, and Xcode Command Line Tools present. CMake, Ninja, ccache, dfu-util, and `idf.py` absent from the default PATH. |
| Python | Default `/usr/bin/python3` was 3.9.6. The Homebrew PATH command selected the installed 3.12.14. |
| ESP-IDF and board | Local `esp-idf/export.sh` absent. `/dev/cu.usbmodem1101` present as a character device. Firmware build, flash, monitor, and physical board behavior remain unverified. |
| Bundled components | Restored into the checkout and verified successfully: M5Unified 0.2.23, M5GFX 0.2.30, and LVGL 8.4.0. |
| Host tests | Transport passed. An imported relative-path defect in the audio runner was corrected; all six codec and packet-duration cases pass. The LVGL simulator was blocked by missing CMake. |

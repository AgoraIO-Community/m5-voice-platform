# Third-party notices

This repository's original firmware and documentation are licensed under the MIT License
(see root `LICENSE`). Redistribution of this tree also includes third-party components
with their own copyright and license terms. Preserve those terms when you redistribute.

## Agora RTSA Lite SDK

- Path: `third_party/agora_rtsa_sdk/`
- Copyright: Agora IO (see `agora_sdk/include/agora_rtc_api.h`)
- Package: see `third_party/agora_rtsa_sdk/PACKAGE_INFO`
- License: proprietary Agora SDK terms. The static libraries and headers are provided
  for building this sample. Redistribution and commercial use remain subject to your
  Agora / Shengwang account agreement and the Agora SDK license. Do not treat the
  root MIT license as covering these binaries.

## Agora Conversational AI Python quickstart (service subtree)

- Path: `service/agent-quickstart-python/`
- Copyright: Agora Community (see `service/agent-quickstart-python/LICENSE`)
- License: MIT

## M5Unified / M5GFX / LVGL (vendor snapshots)

- Path: `vendor/*.tar.xz` (restored into `components/` by `tools/bootstrap_dependencies.py`)
- Upstream: https://github.com/m5stack/M5Unified , https://github.com/m5stack/M5GFX , LVGL
- License: MIT at each library root (see archives / restored `LICENSE` files)
- Notes: these archives are local ESP-IDF 6 compatible snapshots, not unmodified upstream
  tarballs. M5GFX may embed fonts with additional notices (including IPA Font License /
  GFXFF). Keep those embedded notices when redistributing restored sources.
  Details: `vendor/README.md`, `vendor/manifest.json`.

## libg722

- Path: `third_party/libg722/`
- Copyright: Carnegie Mellon University (1993); later contributions by Steve Underwood
  placed in the public domain (see `third_party/libg722/LICENSE`)

## Noto font tooling assets

- Path: `ui_tools/NOTO-FONT-LICENSE.txt`
- License: SIL Open Font License (see that file)

## Espressif ESP-IDF

Firmware builds against ESP-IDF (Apache 2.0). ESP-IDF itself is not vendored in this
repository; install it separately as documented in the README.

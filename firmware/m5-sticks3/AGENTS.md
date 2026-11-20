# RTE2026 Workshop project instructions

This repository is an RTE2026 Workshop example. The checked-in StickS3 board
implementation is a reference target; when another M5Stack board is requested,
reuse the Agora protocol and service flow but verify that board's display,
audio path, buttons, power and M5GFX board profile before editing hardware code.

## Required skills for AI coding agents

- For any Agora, ConvoAI, RTC, RTM, agent lifecycle, token, App Certificate,
  CLI, provider or service configuration task, read
  `skills/agora-skills-main/skills/agora/SKILL.md` first. Follow its routing
  rules and load the linked product reference before changing code.
- For any M5Stack product, pin, board profile, M5Unified, M5GFX, LVGL or
  hardware compatibility task, read `skills/m5stack-assistant/SKILL.md` first.
  Use the official M5Stack MCP when that skill requires product evidence.
- If a task spans both areas, read both skills and keep their boundaries: the
  Agora service owns cloud/session behavior, while the board layer owns local
  audio, display and input adaptation.

The copies under `skills/` are project-local documentation. They do not
automatically register themselves in every AI programming tool. Tools that
honor repository instructions should discover this file; Claude Code, Cursor
and other tools may additionally require their own plugin, rules or workspace
setup described in each skill README.

## Secrets and local configuration

- Never print, commit or upload Wi-Fi passwords, Agora App Certificates, tokens,
  provider API keys or `service/**/server/.env` contents.
- Put firmware-only values in the ignored `main/voice_config_local.h` copied
  from its example. Keep Agora credentials in the ignored service `.env`.
- Before flashing another board, confirm its M5GFX profile, panel size, speaker
  and microphone wiring instead of assuming the StickS3 defaults.

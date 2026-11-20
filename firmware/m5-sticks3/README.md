# M5StickS3 firmware

This is the firmware-only portion of the upstream Agora M5StickS3 voice-agent
recipe. The upstream Python control service is intentionally excluded because
the repository's Next.js application will own the compatible device routes.

Read [`UPSTREAM.md`](UPSTREAM.md) for the original recipe and
[`../../docs/hardware-setup.md`](../../docs/hardware-setup.md) for this
workspace's toolchain, build, and flash instructions. The exact server payloads
are documented in [`../../docs/device-protocol.md`](../../docs/device-protocol.md).

Do not add Agora credentials or provider keys here. The only local firmware
configuration file is ignored `main/voice_config_local.h`, copied from
`main/voice_config.local.example.h`.


## Device controls

Press the right-side B button to cycle through Hermes's installed agents and
Settings. Press front A to open the highlighted agent. Once connected, hold A
to talk and release it to mute the microphone. Replies play while muted. Press
B to leave the conversation. Opening an agent starts a single session that
stays joined across turns.

In Settings, B moves through Volume, Device ID, User, and Back. A on Volume
cycles the six speaker levels. A on Back returns to the library. The Device ID
is the factory MAC in uppercase hexadecimal. User displays the simulated
Hermes account. Settings are accessible offline and with an empty library.

The idle device refreshes its library every 20 seconds and retries failed
requests after 5 seconds. The last successful library stays in memory until
reboot. Menu selection follows the agent ID across refreshes.

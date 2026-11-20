# Audio worker regression

Run from any directory with a host C/C++ compiler:

```sh
bash tests/audio/run.sh
```

This compiles the real `voice_audio.cpp` and the original G.711/G.722 codecs.
FreeRTOS scheduling, queues, M5 peripherals, and time are replaced by a
deterministic asynchronous simulation. The fake speaker retains each submitted
buffer and verifies that its contents stay unchanged until its release
callback. Microphone requests complete after 20 ms, rather than synchronously.

Each of PCM16, G.722, and PCMA is tested with 10 ms and 20 ms packets
(100 and 50 packets/second) against the same timeline. Decoded packets are
coalesced into 40 ms playback chunks so two M5.Unified request slots provide
roughly 80 ms of jitter cushion, with 25 ms partial-chunk flushing at phrase end.

- Continuous remote silence leaves the mic recording and uploading.
- Actual remote speech starts speaker playback with a 60 ms jitter cushion.
- A 450 ms silence hold bridges normal TTS sentence pauses, then releases the
  speaker and resumes capture.
- Stop/restart stops both devices and restores capture without stale playback.
- A microphone DC offset does not trigger VAD, while real AC speech remains
  detectable even when present throughout microphone startup/warmup.
- Packets from a previous session, invalid payloads, and a full packet pool
  cannot start playback or corrupt pool ownership.
- Runtime volume changes are applied by the worker at all six user steps:
  100/80/60/40/20/0 percent maps to M5Unified master values 255/204/153/102/51/0
  without changing microphone capture.
- StopWatch playback applies a bounded peak limiter: quiet speech is lifted,
  while full-scale input is clamped below the configured headroom.

The worker keeps at most 60 ms in one playback buffer and tracks the maximum
worker gap while the speaker is active. These tests exercise worker behavior
and codec integration. They do not verify
real DMA/I2S clocks, ES8311 analog capture, speaker quality, RTOS concurrency,
network timing, or end-to-end Agora recognition. Those require a connected M5
device and the configured service.

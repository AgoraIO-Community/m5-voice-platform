# Locate retained internal allocations

This optional diagnostic is compiled out unless
`CONFIG_HEAP_TRACING_STANDALONE=y`. Set
`CONFIG_HEAP_TRACING_STACK_DEPTH=4` for allocation call stacks. Add
`voice_heap_diag.c` to the main component and call `voice_heap_diag_start()`
for serial `h`, `voice_heap_diag_dump()` for serial `d`.

The default buffer is 768 records allocated in internal RAM only while tracing
(43 KiB with stack depth 4 and no hash map). This can change memory pressure;
move the LVGL pool into PSRAM before enabling it. Keep the watchdog enabled.

1. Start and stop one normal voice session, then leave it idle for 5 seconds.
2. Send `h` to begin `HEAP_TRACE_LEAKS`.
3. Start and stop a second session, then leave it idle for at least 5 seconds
   so task deletion and deferred network cleanup can finish.
4. Send `d`, save the complete output between `HEAPTRACE begin` and
   `HEAPTRACE end`, and retain the exact firmware ELF from this build.

The dump prints allocation sizes, addresses and allocation call stacks, never
allocation contents. It filters the record dump to internal RAM. The summary
still counts records from all memory regions. A nonzero `overflow` means the
trace is incomplete; shorten the run or increase `VOICE_HEAP_TRACE_RECORDS` if
memory allows. Live allocations may be intentional caches: compare equivalent
idle states and repeated runs before identifying a leak.

Every output line uses the `voice_heap` log tag for serial capture. Decode each
colon-separated `callers` code address against the matching ELF:

```sh
xtensa-esp32s3-elf-addr2line -e build/m5stack_agora.elf -pfiaC 0x42000000
```

If the stack stops at the allocator or AOSL wrapper, increase stack depth and
repeat a shorter trace. After diagnosis, disable heap tracing for the normal
firmware; the diagnostic file then contains only unsupported-operation stubs.

## Bundled RTSA 1.9.7 lifecycle finding

The hardware trace taken after a warmed-up start/stop cycle retained a 4096-byte
task stack and 100-byte TCB allocated by `aosl_hal_thread_create`, AOSL queue
objects and locks, and the worker's lwIP DNS thread-local semaphore. The AOSL
portion was approximately 5.2 KiB per engine init/fini cycle. The SDK archive's
DNS implementation submits work to its LTWP pool; its public header also
specifies one `agora_rtc_init` per process.

The transport therefore initializes the engine once per boot, creates and
destroys each RTC connection, and starts/stops the cloud Agent each session.
Each session obtains a fresh token. It validates the App ID and region against
the initialized engine and requires a reboot if either changes. This avoids
repeatedly exercising the leaking engine teardown; retained engine memory is
intentional. Repeat the same idle-state trace across multiple sessions to
verify that the retained memory remains bounded.

The separate local microphone/speaker exercise retained only first-use
semaphores and display/network caches, with no comparable per-cycle audio
allocation growth.

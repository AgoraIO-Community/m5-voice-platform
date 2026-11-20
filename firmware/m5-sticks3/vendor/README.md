# Pinned display and board components

These source archives preserve the exact local component trees used by the
StickS3 firmware. M5Unified and M5GFX contain local ESP-IDF 6 and audio lifecycle
fixes. Their declared versions identify the library family; an upstream package
with the same version is **not** an equivalent replacement. The archive SHA-256
and per-file SHA-256 values in `manifest.json` identify these snapshots.

From this project root, restore the bundled sources with Python 3:

```sh
python3 tools/bootstrap_dependencies.py
python3 tools/bootstrap_dependencies.py --check
```

The bootstrap runs offline, verifies every archive before extraction, and
verifies every restored file. An existing source tree with local changes is
left untouched; preserve and move it aside before restoring. `.git`
administration files are excluded. No build artifacts or device credentials
are included. Empty source directories are preserved in the archives.

`manifest.json` records upstream URLs, the source-tree locations used when the
snapshot was prepared (with environment-specific prefixes omitted), declared
versions and included license files. M5Unified, M5GFX and LVGL use the
MIT license at their library roots. M5GFX also contains fonts with their own
notices, including the IPA font license and the GFXFF notice. Preserve all
embedded font notices when redistributing these sources.

When intentionally updating a component, regenerate its archive, archive hash,
and file-hash manifest together, and repeat the IDF build and device audio
tests. Do not replace these archives with upstream downloads based only on a
matching version number.

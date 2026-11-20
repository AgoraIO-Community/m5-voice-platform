#!/usr/bin/env python3
"""Build reproducible, subset LVGL fonts from product strings.

Collects the characters a product actually shows, verifies the source font covers
them, subsets the font (TTF, OTF, or one face of a TTC collection), converts each size
with a pinned lv_font_conv, and writes a manifest that records inputs and results.

Examples:
  # Chinese UI font from the strings used in source files and a strings table
  python3 scripts/make_lvgl_font.py \
      --font /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc --face "Noto Sans CJK SC" \
      --name ui_font_cjk --sizes 12,14,16,20 --ascii \
      --scan assets/lvgl-ui/templates --strings i18n/zh.json \
      --license OFL-1.1 --out assets/lvgl-ui/fonts

  # Monospaced ASCII font for numeric readouts
  python3 scripts/make_lvgl_font.py --font /usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf \
      --name ui_font_mono --sizes 12,14,16,28,40 --ascii --symbols "°…•" --license OFL-1.1 \
      --out assets/lvgl-ui/fonts

Requires Python fontTools and Node.js (npx downloads lv_font_conv at the pinned version).
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

from fontTools import subset
from fontTools.ttLib import TTCollection, TTFont

LV_FONT_CONV = "lv_font_conv@1.5.2"
CODE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".ino"}
TEXT_SUFFIXES = {".txt", ".json", ".po", ".csv", ".yaml", ".yml", ".md"}
STRING_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def collect_from_file(path):
    text = path.read_text(encoding="utf-8")
    if path.suffix in CODE_SUFFIXES:
        return "".join(match.group(1) for match in STRING_LITERAL.finditer(text))
    return text


def collect_characters(args):
    chars = set()
    sources = []
    for root in args.scan or []:
        root = Path(root)
        files = [root] if root.is_file() else sorted(
            p for p in root.rglob("*") if p.suffix in CODE_SUFFIXES | TEXT_SUFFIXES)
        for path in files:
            chars.update(collect_from_file(path))
            sources.append(str(path))
    for path in args.strings or []:
        chars.update(Path(path).read_text(encoding="utf-8"))
        sources.append(str(path))
    if args.ascii:
        chars.update(chr(code) for code in range(0x20, 0x7F))
    chars.update(args.symbols or "")
    printable = {c for c in chars if c.isprintable() and c not in "\t\r\n"}
    if not args.ascii:
        printable = {c for c in printable if ord(c) >= 0x80} | set(args.symbols or "")
    return "".join(sorted(printable)), sources


def load_face(font_path, face_name):
    if font_path.suffix.lower() in {".ttc", ".otc"}:
        collection = TTCollection(str(font_path))
        names = []
        for font in collection.fonts:
            family = font["name"].getDebugName(1) or ""
            full = font["name"].getDebugName(4) or ""
            names.append(family)
            if face_name in (family, full):
                return font
        raise SystemExit(f"Face {face_name!r} not in {font_path}; available: {sorted(set(names))}")
    return TTFont(str(font_path))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--font", type=Path, required=True)
    parser.add_argument("--face", help="Family or full name inside a .ttc/.otc collection")
    parser.add_argument("--name", required=True, help="C symbol prefix, e.g. ui_font_cjk")
    parser.add_argument("--sizes", required=True, help="Comma-separated pixel sizes")
    parser.add_argument("--bpp", type=int, default=4, choices=(1, 2, 3, 4, 8))
    parser.add_argument("--scan", nargs="*", help="Files/directories; string literals in code are collected")
    parser.add_argument("--strings", nargs="*", help="UTF-8 files whose full text is collected")
    parser.add_argument("--ascii", action="store_true", help="Include printable ASCII 0x20-0x7E")
    parser.add_argument("--symbols", help="Extra characters to include")
    parser.add_argument("--license", required=True, help="Source font license identifier, e.g. OFL-1.1")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--allow-missing", action="store_true")
    args = parser.parse_args()

    if not shutil.which("npx"):
        raise SystemExit("npx not found; install Node.js to run lv_font_conv")
    chars, sources = collect_characters(args)
    if not chars:
        raise SystemExit("No characters collected; pass --scan, --strings, --ascii, or --symbols")

    face = load_face(args.font, args.face) if args.face else load_face(args.font, "")
    cmap = face.getBestCmap()
    missing = [c for c in chars if ord(c) not in cmap and c != " "]
    if missing and not args.allow_missing:
        shown = " ".join(f"{c}(U+{ord(c):04X})" for c in missing[:40])
        raise SystemExit(f"{len(missing)} characters are missing from the font: {shown}")
    covered = "".join(c for c in chars if c not in missing)

    args.out.mkdir(parents=True, exist_ok=True)
    outputs = []
    with tempfile.TemporaryDirectory() as tmp:
        subset_path = Path(tmp) / "subset.otf"
        options = subset.Options()
        options.layout_features = ["kern"]
        options.name_IDs = ["*"]
        options.notdef_outline = True
        subsetter = subset.Subsetter(options)
        subsetter.populate(unicodes=[ord(c) for c in covered])
        subsetter.subset(face)
        face.save(str(subset_path))
        codepoints = ",".join(str(ord(c)) for c in covered)
        for size in (int(s) for s in args.sizes.split(",")):
            target = args.out / f"{args.name}_{size}.c"
            command = ["npx", "-y", LV_FONT_CONV, "--font", str(subset_path), "-r", codepoints,
                       "--size", str(size), "--bpp", str(args.bpp), "--format", "lvgl",
                       "--lv-include", "lvgl.h", "--no-compress", "-o", str(target)]
            subprocess.run(command, check=True, capture_output=True, text=True)
            source = target.read_text(encoding="utf-8")
            source = source.replace(str(subset_path), f"{args.font.name} (subset)")
            target.write_text(source, encoding="utf-8")
            outputs.append({"size": size, "file": target.name, "bytes": target.stat().st_size})

    header = args.out / f"{args.name}.h"
    header.write_text(
        "#pragma once\n#include \"lvgl.h\"\n\n" +
        "".join(f"LV_FONT_DECLARE({args.name}_{o['size']})\n" for o in outputs), encoding="utf-8")
    manifest = {
        "name": args.name,
        "tool": LV_FONT_CONV,
        "source_font": args.font.name,
        "source_face": args.face,
        "source_sha256": hashlib.sha256(args.font.read_bytes()).hexdigest(),
        "license": args.license,
        "bpp": args.bpp,
        "glyph_count": len(covered),
        "characters": covered,
        "missing": "".join(missing),
        "sources": sources,
        "outputs": outputs,
        "rebuild": shlex.join(["python3", "ui_tools/make_lvgl_font.py"] + sys.argv[1:]),
    }
    (args.out / f"{args.name}.manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in manifest.items() if k not in ("characters", "sources")},
                     ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()

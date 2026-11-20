#!/usr/bin/env python3
"""Convert the simulator's actual RGB565 output into reviewable artifacts."""
from pathlib import Path
import math
import sys
from PIL import Image, ImageDraw

directory = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent / "captures"
for ppm in directory.glob("*.ppm"):
    Image.open(ppm).save(ppm.with_suffix(".png"))
files = sorted(directory.glob("[0-9][0-9]-*.png"))
cols, cell_w, cell_h = 5, 290, 518
sheet = Image.new("RGB", (cols * cell_w, math.ceil(len(files) / cols) * cell_h), "#101b29")
draw = ImageDraw.Draw(sheet)
for i, path in enumerate(files):
    x, y = (i % cols) * cell_w + 10, (i // cols) * cell_h + 24
    sheet.paste(Image.open(path).resize((270, 480), Image.Resampling.NEAREST), (x, y))
    draw.text((x, y - 18), path.stem, fill="white")
sheet.save(directory / "states-contact-sheet.png")
# Compact review of the actual firmware-rendered expressions, at 2x pixels.
preview_states = ("01-idle", "07-listening", "08-thinking", "09-speaking")
preview = Image.new("RGB", (len(preview_states) * 282 + 12, 504), "#101b29")
for i, name in enumerate(preview_states):
    frame = Image.open(directory / f"{name}.ppm")
    preview.paste(frame.resize((270, 480), Image.Resampling.NEAREST), (12 + i * 282, 12))
preview.save(directory / "expression-preview.png")
for name in ("idle", "connect", "speak"):
    frames = [Image.open(path).resize((405, 720), Image.Resampling.NEAREST)
              for path in sorted(directory.glob(f"motion-{name}-*.png"))]
    frames[0].save(directory / f"motion-{name}.gif", save_all=True, append_images=frames[1:], duration=160, loop=0)
print(directory / "states-contact-sheet.png")

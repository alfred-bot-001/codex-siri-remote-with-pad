"""Build monochrome LVGL A8 app icons from the checked-in SVG artwork.

Generation dependency: pip install cairosvg pillow
The generated C file is checked in, so firmware builds need no Python packages.
"""

from io import BytesIO
from pathlib import Path

import cairosvg
from PIL import Image


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "design" / "icons"
OUTPUT = ROOT / "firmware" / "main" / "app_icons.c"
SIZE = 56
ICONS = {
    "openai": "app_icon_openai",
    "claude": "app_icon_claude",
    "googlechrome": "app_icon_chrome",
}


def make_icon(source: Path, name: str) -> str:
    png = cairosvg.svg2png(url=str(source), output_width=SIZE, output_height=SIZE)
    alpha = Image.open(BytesIO(png)).convert("RGBA").getchannel("A").tobytes()
    lines = [f"static const uint8_t {name}_pixels[{len(alpha)}] = {{"]
    for offset in range(0, len(alpha), 16):
        lines.append("    " + ", ".join(f"0x{pixel:02x}" for pixel in alpha[offset : offset + 16]) + ",")
    lines += [
        "};",
        f"const lv_image_dsc_t {name} = {{",
        "    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,",
        f"               .w = {SIZE}, .h = {SIZE}, .stride = {SIZE}}},",
        f"    .data_size = sizeof({name}_pixels),",
        f"    .data = {name}_pixels,",
        "};",
    ]
    return "\n".join(lines)


OUTPUT.write_text(
    '#include "app_icons.h"\n\n'
    + "\n\n".join(make_icon(SOURCE / f"{file}.svg", name) for file, name in ICONS.items())
    + "\n",
    encoding="utf-8",
)

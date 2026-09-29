"""Rebuild the OFL Source Han Sans subset used by the P4 settings UI."""
import hashlib
import pathlib
import re
import subprocess
import sys

project = pathlib.Path(__file__).resolve().parents[1]
source = project / "managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf"
output = project / "main/font_cn28.c"
html = project.parents[0] / "design/index.html"
converter = sys.argv[1] if len(sys.argv) > 1 else "lv_font_conv"
if hashlib.sha256(source.read_bytes()).hexdigest() != "1ee89e1669362dee13851129c0a8a791a87521eb4148e5efbf5d26596738e25b":
    raise SystemExit("The Source Han Sans file differs from the pinned LVGL copy")
text = html.read_text() + (project / "main/main.c").read_text()
symbols = "".join(sorted(set(re.findall(r"[\u3400-\u9fff]", text))))
subprocess.run([converter, "--font", str(source), "--size", "28", "--bpp", "4", "--format", "lvgl",
                "--range", "0x20-0x7E", "--symbols", symbols, "--no-compress", "--lv-include",
                "lvgl.h", "--lv-font-name", "font_cn28", "-o", str(output)], check=True)
lines = output.read_text().splitlines()
lines = [" * Source: Source Han Sans SC, SIL OFL 1.1; see licenses/SourceHanSansSC-OFL.txt" if line.startswith(" * Opts:") else line for line in lines]
output.write_text("\n".join(lines) + "\n")
print(f"Generated {len(symbols)} Chinese glyphs")

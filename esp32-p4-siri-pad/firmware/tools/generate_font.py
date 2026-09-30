"""Rebuild the OFL Source Han Sans Bold subset used by the P4 UI."""
import hashlib
import pathlib
import re
import subprocess
import sys
import tempfile
import urllib.request

project = pathlib.Path(__file__).resolve().parents[1]
source = pathlib.Path(tempfile.gettempdir()) / "SourceHanSansSC-Bold.otf"
source_url = "https://raw.githubusercontent.com/adobe-fonts/source-han-sans/release/OTF/SimplifiedChinese/SourceHanSansSC-Bold.otf"
source_sha256 = "df2b90f5bcc6d01dfc964cec5f6d535d6b6aebd26ed7fd79a9c1b3f2112fcb6b"
size = int(sys.argv[2]) if len(sys.argv) > 2 else 28
if size not in (18, 28):
    raise SystemExit("Supported font sizes: 18, 28")
output = project / f"main/font_cn{size}.c"
html = project.parents[0] / "design/index.html"
converter = sys.argv[1] if len(sys.argv) > 1 else "lv_font_conv"
if not source.exists():
    urllib.request.urlretrieve(source_url, source)
if hashlib.sha256(source.read_bytes()).hexdigest() != source_sha256:
    raise SystemExit("The Source Han Sans Bold file differs from the pinned Adobe copy")
text = html.read_text() + "".join(p.read_text() for p in (project / "main").glob("*.c") if not p.name.startswith("font_cn"))
symbols = "".join(sorted(set(re.findall(r"[\u2000-\u206f\u3000-\u303f\u3400-\u9fff\uff00-\uffef]", text))))
subprocess.run([converter, "--font", str(source), "--size", str(size), "--bpp", "4", "--format", "lvgl",
                "--range", "0x20-0x7E", "--symbols", symbols, "--no-compress", "--lv-include",
                "lvgl.h", "--lv-font-name", f"font_cn{size}", "-o", str(output)], check=True)
lines = output.read_text().splitlines()
lines = [" * Source: Source Han Sans SC Bold, SIL OFL 1.1; see licenses/SourceHanSansSC-OFL.txt" if line.startswith(" * Opts:") else line for line in lines]
output.write_text("\n".join(lines).rstrip() + "\n")
print(f"Generated {len(symbols)} CJK and punctuation glyphs")

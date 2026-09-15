#!/usr/bin/env python3
from pathlib import Path
from PIL import Image
import base64

src = Path(
    "/Users/bala_mani/.cursor/projects/Users-bala-mani-Projects-MyProjects-yuga/assets/4104036f-c043-4e77-a6d7-b10b117683cb.png"
)
icons = Path("/Users/bala_mani/Projects/MyProjects/yuga/editors/zed/icons")
img = Image.open(src).convert("RGBA")
pixels = img.load()
w, h = img.size
for y in range(h):
    for x in range(w):
        r, g, b, a = pixels[x, y]
        # White / near-white -> transparent; keep the black silhouette.
        if r > 240 and g > 240 and b > 240:
            pixels[x, y] = (0, 0, 0, 0)
        else:
            pixels[x, y] = (0, 0, 0, a if a else 255)

png_path = icons / "yuga.png"
img.save(png_path)
buf = Path("/tmp/yuga_icon.png")
img.save(buf)
b64 = base64.b64encode(buf.read_bytes()).decode("ascii")
(icons / "yuga.svg").write_text(
    f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}">
  <image href="data:image/png;base64,{b64}" width="{w}" height="{h}"/>
</svg>
''',
    encoding="utf-8",
)
print(f"icon {w}x{h} -> {png_path}")

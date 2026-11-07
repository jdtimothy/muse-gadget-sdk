#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Render the avatar's reactions, and the states they're judged against, into a gallery page.

    python tools/muse/react_gallery.py OUT_DIR

Builds tools/muse/react_anim.c against avatar/muse_pixel.c with $CC (default cc),
and writes OUT_DIR/gallery.html with one PNG strip per animation, played by CSS.
Needs no Python packages.
"""
import os
import shlex
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FRAME_MS = 40
REFERENCE = ["idle", "listening", "thinking", "speaking", "happy", "error"]
SCENES = ["dance", "stretch", "hum", "butterfly", "doze", "pace"]


def png(path, width, height, rows):
    """Writes RGB `rows` (one bytes object a row) as a PNG."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + r for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def render(out):
    """Renders every animation to OUT/<name>.png; returns [(name, frames, size)]."""
    cc = shlex.split(os.environ.get("CC", "cc"))
    os.makedirs(out, exist_ok=True)
    anims = []
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "react_anim.exe" if os.name == "nt" else "react_anim")
        subprocess.run([*cc, "-O2", "-Wall", "-I", "components/muse", "tools/muse/react_anim.c",
                        "avatar/muse_pixel.c", "-lm", "-o", exe], cwd=ROOT, check=True, capture_output=True, text=True)
        listing = subprocess.run([exe, tmp], check=True, capture_output=True, text=True).stdout
        for line in listing.splitlines():
            if not line.strip():
                continue
            name, frames, size = line.split()
            frames, size = int(frames), int(size)
            with open(os.path.join(tmp, name + ".rgb"), "rb") as f:
                data = f.read()
            row, frame = size * 3, size * size * 3
            rows = [b"".join(data[i * frame + y * row:i * frame + (y + 1) * row] for i in range(frames))
                    for y in range(size)]
            png(os.path.join(out, name + ".png"), size * frames, size, rows)
            anims.append((name, frames, size))
    return anims


def tile(name, frames, size):
    label = name.replace("speaking-", "speaking + ").replace("thinking-", "thinking + ")
    return (f'<figure class="tile"><div class="anim" style="--frames:{frames};--dur:{frames * FRAME_MS}ms">'
            f'<img src="{name}.png" alt="{label}" width="{size * frames}" height="{size}"></div>'
            f'<figcaption>{label}</figcaption></figure>')


# Published as an Artifact, which adds the document skeleton: the page starts at <title>.
PAGE = """<title>Muse Reactions</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Atkinson+Hyperlegible:wght@400;700&family=Silkscreen&display=swap">
<style>
/* A contact sheet of animation tiles on the device's own black, grouped reference / reactions / blends. */
:root {{ --bg: #f4f2fa; --fg: #1d1930; --muted: #5f5a76; --tile: #000; --line: #d9d4e8; --accent: #6a45d9;
         --display: "Silkscreen", ui-monospace, monospace;
         --body: "Atkinson Hyperlegible", system-ui, -apple-system, "Segoe UI", sans-serif; }}
@media (prefers-color-scheme: dark) {{ :root:not([data-theme="light"]) {{ --bg: #0e0c16; --fg: #ece8f8; --muted: #a39dbd;
         --line: #2a2640; --accent: #a77dff; color-scheme: dark; }} }}
:root[data-theme="dark"] {{ --bg: #0e0c16; --fg: #ece8f8; --muted: #a39dbd; --line: #2a2640; --accent: #a77dff;
         color-scheme: dark; }}
* {{ box-sizing: border-box; }}
body {{ margin: 0; background: var(--bg); color: var(--fg); font: 16px/1.5 var(--body); }}
main {{ max-width: 1100px; margin: 0 auto; padding-block: 28px 48px; padding-inline: 16px; }}
h1 {{ font: 400 28px/1.2 var(--display); color: var(--accent); margin: 0 0 6px; text-wrap: balance; }}
h2 {{ font-size: 17px; margin: 36px 0 2px; text-wrap: balance; }}
p {{ margin: 0; color: var(--muted); max-width: 65ch; }}
.grid {{ display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 16px; margin-top: 14px; }}
.tile {{ margin: 0; min-width: 0; }}
.anim {{ position: relative; overflow: hidden; width: 100%; aspect-ratio: 1; background: var(--tile);
         border-radius: 12px; border: 1px solid var(--line); }}
/* The strip is --frames tiles wide; steps() lands on whole frames, the last one included. */
.anim img {{ position: absolute; left: 0; top: 0; height: 100%; width: calc(var(--frames) * 100%); max-width: none;
            image-rendering: pixelated; animation: play var(--dur) steps(var(--frames)) infinite; }}
@keyframes play {{ to {{ transform: translateX(-100%); }} }}
@media (prefers-reduced-motion: reduce) {{ .anim img {{ animation-duration: calc(var(--dur) * 3); }} }}
figcaption {{ margin-top: 6px; font-size: 14px; text-align: center; }}
</style>
<main>
<h1>Muse reactions</h1>
<p>Drawn by the gadget's own renderer on its 64 by 64 grid, 25 frames a second, at the full-size avatar's scale. Each reaction comes in, holds 4 s and eases out. The gadget's own scenes play their own length; the dance shows its first 4 s of 30.</p>
<h2>The gadget's own scenes</h2>
<p>The dance, for 30 s after a media event, and the idle scenes, one every 10-30 s while nothing is happening.</p>
<div class="grid">{scenes}</div>
<h2>The avatar today</h2>
<p>For comparison: what reactions should feel like they belong with.</p>
<div class="grid">{reference}</div>
<h2>Reactions</h2>
<div class="grid">{reactions}</div>
<h2>Reactions while speaking</h2>
<p>The face, glow and prop show; the mouth keeps talking.</p>
<div class="grid">{blends}</div>
<h2>Reactions while thinking</h2>
<p>Muse often reacts while it's still working on a reply. The reaction's face replaces the thinking face, and its prop takes the thought dots' place.</p>
<div class="grid">{thinks}</div>
</main>
"""


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = os.path.abspath(sys.argv[1])
    try:
        anims = render(out)
    except subprocess.CalledProcessError as e:
        sys.exit((e.stdout or "") + (e.stderr or "") + "react_anim doesn't build or run")
    by = {a[0]: a for a in anims}
    reference = "".join(tile(*by[n]) for n in REFERENCE)
    blend = ("speaking-", "thinking-")
    scenes = "".join(tile(*by[n]) for n in SCENES if n in by)
    reactions = "".join(tile(*a) for a in anims
                        if a[0] not in REFERENCE and a[0] not in SCENES and not a[0].startswith(blend))
    blends = "".join(tile(*a) for a in anims if a[0].startswith("speaking-"))
    thinks = "".join(tile(*a) for a in anims if a[0].startswith("thinking-"))
    with open(os.path.join(out, "gallery.html"), "w", encoding="utf-8") as f:
        f.write(PAGE.format(scenes=scenes, reference=reference, reactions=reactions, blends=blends, thinks=thinks))
    print(os.path.join(out, "gallery.html"))
    for name, frames, _ in anims:
        print(f"{name}.png  ({frames} frames)")


if __name__ == "__main__":
    main()

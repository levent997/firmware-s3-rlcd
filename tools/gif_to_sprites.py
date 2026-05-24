#!/usr/bin/env python3
"""
Convert clawd-on-desk GIFs into a packed C header of monochrome bitmap frames
for u8g2's drawXBM().

For each input gif:
  - Iterate frames, composite onto a white background (handles disposal)
  - Crop to the union bounding box of non-white pixels across ALL frames
    (so the character stays still relative to its bbox)
  - Resize to fit TARGET x TARGET keeping aspect, center on canvas
  - Threshold to 1bpp (alpha > 0 AND not pure-white => 1)
  - Emit XBM-style byte array

Output: firmware-s3-rlcd/src/sprites.h
"""
import os, sys, struct
from pathlib import Path
from PIL import Image, ImageSequence

HERE = Path(__file__).resolve().parent
GIF_DIR = HERE.parent.parent / "clawd-on-desk" / "assets" / "gif"
OUT_FILE = HERE.parent / "src" / "sprites.h"

TARGET = 128           # output canvas width and height (px)
MAX_FRAMES = 8         # cap to keep flash usage reasonable

# Name in C => filename. Order matters only for readability.
SPRITES = [
    ("idle",         "clawd-idle.gif"),
    ("building",     "clawd-building.gif"),    # used for WORKING state (better than typing)
    ("typing",       "clawd-typing.gif"),
    ("thinking",     "clawd-thinking.gif"),
    ("happy",        "clawd-happy.gif"),
    ("notification", "clawd-notification.gif"),
    ("error",        "clawd-error.gif"),
    ("sleeping",     "clawd-sleeping.gif"),
]

def composite_frames(im):
    """Yield RGBA frames with proper disposal handling."""
    base = Image.new("RGBA", im.size, (255, 255, 255, 255))
    for frame in ImageSequence.Iterator(im):
        f = frame.convert("RGBA")
        composed = base.copy()
        composed.alpha_composite(f)
        yield composed
        # Most GIFs in this project are disposal=2 (restore to background);
        # PIL handles this internally via the next frame's tile, so we keep
        # `base` as a fresh white canvas each time.

def union_bbox(frames):
    """Bounding box covering content across all frames (non-near-white)."""
    union = None
    for f in frames:
        # alpha+brightness mask: pixel is "content" if not near white
        rgba = f.load()
        w, h = f.size
        # Use point() trick: compute luminance, threshold
        gray = f.convert("L")
        # invert: content = dark area = high in inverted
        inv = gray.point(lambda v: 255 if v < 240 else 0)
        bbox = inv.getbbox()
        if bbox is None:
            continue
        if union is None:
            union = bbox
        else:
            union = (min(union[0], bbox[0]),
                     min(union[1], bbox[1]),
                     max(union[2], bbox[2]),
                     max(union[3], bbox[3]))
    return union

def to_mono(rgba):
    """RGBA -> 1bpp PIL image.

    Only saturated, mid-luminance pixels become ink (the salmon body of Clawd).
    Pure white = background. Pure black (eyes, mouth) = ALSO background, so
    they show as holes in the body silhouette on the device — giving the
    character its face. Transparent areas are background.

    PIL '1' convention: 0 = black ink, 255 = white. We start the canvas at 1
    (background) and paint 0 where we want ink.
    """
    w, h = rgba.size
    out = Image.new("1", (w, h), 1)
    px_in = rgba.load()
    px_out = out.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px_in[x, y]
            if a < 64:
                continue
            # too bright = background
            if r > 230 and g > 230 and b > 230:
                continue
            # too dark = background (eyes / mouth / outlines that we want to "see through")
            if r < 60 and g < 60 and b < 60:
                continue
            # mid-tone colored pixel = body, draw as ink
            px_out[x, y] = 0
    return out

def fit_centered(mono, size):
    """Place mono into a size x size canvas keeping aspect, centered."""
    w, h = mono.size
    s = min(size / w, size / h)
    nw, nh = max(1, int(round(w * s))), max(1, int(round(h * s)))
    resized = mono.resize((nw, nh), Image.NEAREST)
    canvas = Image.new("1", (size, size), 1)  # white background
    canvas.paste(resized, ((size - nw) // 2, (size - nh) // 2))
    return canvas

def to_xbm_bytes(mono):
    """Convert PIL '1' image to XBM byte order (LSB-first per byte, row-major)."""
    w, h = mono.size
    assert w % 8 == 0
    out = bytearray()
    px = mono.load()
    for y in range(h):
        for bx in range(w // 8):
            b = 0
            for bit in range(8):
                x = bx * 8 + bit
                # In '1' mode, 0 = black (set), 255 = white (clear) by PIL convention,
                # but our threshold made dark=0 ink. We want set bit when ink.
                if px[x, y] == 0:
                    b |= (1 << bit)
            out.append(b)
    return bytes(out)

def convert_gif(path):
    im = Image.open(path)
    frames = list(composite_frames(im))
    if not frames:
        return []
    bbox = union_bbox(frames)
    if bbox is None:
        bbox = (0, 0, im.size[0], im.size[1])

    # Add a small margin so we don't crop the sprite edges
    pad = 4
    x0, y0, x1, y1 = bbox
    x0 = max(0, x0 - pad); y0 = max(0, y0 - pad)
    x1 = min(im.size[0], x1 + pad); y1 = min(im.size[1], y1 + pad)
    cropped = [f.crop((x0, y0, x1, y1)) for f in frames]

    # Subsample frames evenly to MAX_FRAMES
    if len(cropped) > MAX_FRAMES:
        idx = [round(i * (len(cropped) - 1) / (MAX_FRAMES - 1)) for i in range(MAX_FRAMES)]
        cropped = [cropped[i] for i in idx]

    out = []
    for f in cropped:
        mono = to_mono(f)
        fitted = fit_centered(mono, TARGET)
        out.append(to_xbm_bytes(fitted))
    return out

def emit_header(sprite_data, file):
    file.write('// Auto-generated by tools/gif_to_sprites.py. Do not edit by hand.\n')
    file.write('#pragma once\n')
    file.write('#include <stdint.h>\n')
    file.write('#ifdef __cplusplus\n')
    file.write('#include <pgmspace.h>\n')
    file.write('#endif\n\n')
    file.write(f'constexpr int SPRITE_W = {TARGET};\n')
    file.write(f'constexpr int SPRITE_H = {TARGET};\n')
    file.write(f'constexpr int SPRITE_BYTES = {TARGET * TARGET // 8};\n\n')

    # write frames as flat array per sprite
    sprite_names = []
    for name, frames in sprite_data:
        sprite_names.append((name, len(frames)))
        file.write(f'// {name}: {len(frames)} frames\n')
        file.write(f'static const uint8_t SPRITE_{name.upper()}[{len(frames)}][SPRITE_BYTES] PROGMEM = {{\n')
        for fi, frame in enumerate(frames):
            file.write('  {')
            for i, b in enumerate(frame):
                if i % 16 == 0:
                    file.write('\n    ')
                file.write(f'0x{b:02x},')
            file.write('\n  },\n')
        file.write('};\n\n')

    file.write('enum SpriteId {\n')
    for i, (n, _) in enumerate(sprite_names):
        file.write(f'  SPR_{n.upper()} = {i},\n')
    file.write(f'  SPR_COUNT = {len(sprite_names)}\n')
    file.write('};\n\n')

    file.write('struct SpriteInfo {\n')
    file.write('  const uint8_t (*frames)[SPRITE_BYTES];\n')
    file.write('  uint8_t frame_count;\n')
    file.write('};\n\n')

    file.write('static const SpriteInfo SPRITES[SPR_COUNT] = {\n')
    for n, c in sprite_names:
        file.write(f'  {{ SPRITE_{n.upper()}, {c} }},\n')
    file.write('};\n')

def main():
    sprite_data = []
    for name, fname in SPRITES:
        path = GIF_DIR / fname
        if not path.exists():
            print(f"WARN: missing {path}, skipping", file=sys.stderr)
            continue
        print(f"Converting {fname} ...")
        frames = convert_gif(path)
        print(f"  -> {len(frames)} frames, {TARGET}x{TARGET} 1bpp")
        sprite_data.append((name, frames))

    OUT_FILE.parent.mkdir(exist_ok=True, parents=True)
    with open(OUT_FILE, 'w', encoding='ascii') as f:
        emit_header(sprite_data, f)
    print(f"Wrote {OUT_FILE}")
    total = sum(len(fr) * (TARGET * TARGET // 8) for _, fr in sprite_data)
    print(f"Total sprite bytes: {total}")

if __name__ == '__main__':
    main()

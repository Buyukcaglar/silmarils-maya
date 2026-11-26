"""make_icon.py - builds the application icons from the icon artwork (res/maya64-icon.png, a square RGBA PNG).

Writes a Windows .ico with PNG images of 16, 20, 24, 32, 40, 48, 64, 128 and 256 pixels, and optionally the
1024-pixel PNG that the macOS host's .icns is made from.  Needs Pillow.

    python tools/make_icon.py <artwork.png> <out.ico> [out_1024.png]
"""
import io
import struct
import sys

from PIL import Image, ImageFilter

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def square(im):
    """The artwork on a transparent square canvas, centred."""
    im = im.convert('RGBA')
    side = max(im.size)
    if im.size == (side, side):
        return im
    canvas = Image.new('RGBA', (side, side), (0, 0, 0, 0))
    canvas.paste(im, ((side - im.width) // 2, (side - im.height) // 2))
    return canvas


def scaled(im, size):
    """`im` reduced to size x size.  The colours are scaled premultiplied, so transparent edges keep no fringe."""
    out = im.convert('RGBa').resize((size, size), Image.LANCZOS).convert('RGBA')
    if size <= 48:
        # small sizes lose the artwork's fine detail; a light sharpen keeps the outlines readable
        out = out.filter(ImageFilter.UnsharpMask(radius=0.6, percent=60, threshold=0))
    return out


def png_bytes(im):
    buf = io.BytesIO()
    im.save(buf, 'PNG', optimize=True)
    return buf.getvalue()


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    art = square(Image.open(sys.argv[1]))
    images = [png_bytes(scaled(art, s)) for s in SIZES]
    head = struct.pack('<HHH', 0, 1, len(SIZES))
    offset = 6 + 16 * len(SIZES)
    dirs = b''
    for s, data in zip(SIZES, images):
        dirs += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(sys.argv[2], 'wb') as f:
        f.write(head + dirs + b''.join(images))
    if len(sys.argv) > 3:
        scaled(art, 1024).save(sys.argv[3], 'PNG', optimize=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())

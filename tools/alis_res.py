"""ALIS (Silmarils, v1.x engine) script/resource reader for Le Fetiche Maya.
Handles Amiga (.CO, big-endian) and PC (.IO, little-endian) packed scripts."""
import struct, os

def rle_unpack_strided(src, out_size, modpack):
    """Old Silmarils RLE with output stride `modpack` (column-interleaved), as in typepack 0xC0."""
    out = bytearray(out_size); i = 0; pu = 0; cnt = modpack
    def put(v):
        nonlocal pu, cnt
        if pu >= out_size:
            cnt -= 1
            if cnt < 1: return False
            pu = modpack - cnt
        out[pu] = v; pu += modpack; return True
    while i < len(src) and cnt > 0:
        b = src[i]; i += 1; n = b & 0x7f
        if b & 0x80:
            v = src[i] if i < len(src) else 0; i += 1
            for _ in range(n):
                if not put(v): break
        else:
            for _ in range(n):
                if not put(src[i] if i < len(src) else 0): break
                i += 1
    return bytes(out)

def rle_unpack(src, out_size):
    out = bytearray(); i = 0
    while i < len(src) and len(out) < out_size:
        b = src[i]; i += 1
        n = b & 0x7f
        if b & 0x80:
            out += bytes([src[i] if i < len(src) else 0]) * n; i += 1
        else:
            out += src[i:i+n]; i += n
    return bytes(out[:out_size]).ljust(out_size, b'\0')

class Script:
    def __init__(self, path):
        self.path = path; self.name = os.path.basename(path)
        raw = open(path, 'rb').read()
        self.le = path.upper().endswith('.IO')
        e = '<' if self.le else '>'
        self.e = e
        magic = struct.unpack(e + 'I', raw[:4])[0]
        self.typepack = magic >> 24
        self.size = magic & 0xffffff
        self.is_main = struct.unpack(e + 'H', raw[4:6])[0] == 0
        off = 6; size = self.size - 6
        if self.is_main:
            self.vmspecs = raw[6:22]; off = 22; size -= 16
        modpack = 8 if (not self.le and self.typepack & 0x40) else 1
        if self.typepack & 0x80 and modpack > 1:
            self.data = rle_unpack_strided(raw[off:], size, modpack)
        elif self.typepack & 0x80:
            self.data = rle_unpack(raw[off:], size)
        else:
            self.data = raw[off:off+size]
        self.raw = raw
    def u8(self, o): return self.data[o]
    def u16(self, o): return struct.unpack_from(self.e + 'H', self.data, o)[0]
    def s16(self, o): return struct.unpack_from(self.e + 'h', self.data, o)[0]
    def u32(self, o): return struct.unpack_from(self.e + 'I', self.data, o)[0]

    # ---- resource table ----
    def resources(self):
        """yield (index, offset, bytes-view-start) for each entry of the sprite/resource table"""
        l = self.u32(0xe)
        n = self.u16(l + 4)
        tbl = l + self.u32(l)
        out = []
        for i in range(n):
            a = tbl + i * 4
            at = a + self.u32(a)
            out.append((i, at))
        return out

    def res_header(self, at):
        t = self.data[at]
        w = self.u16(at + 2) + 1
        h = self.u16(at + 4) + 1
        return t, self.data[at+1], w, h

AMIGA_DIR = os.path.join(os.path.dirname(__file__), '..', '_extracted', 'amiga', 'Maya', 'data')
DOS_DIR = os.path.join(os.path.dirname(__file__), '..', '_extracted', 'dos', 'le-fetiche-maya', 'MAYA')

# ---------------- bitmap decoding ----------------
def decode_bitmap(s, at):
    """Return (type, w, h, bpp, rows-of-indices).
    Types 0/2 (both ports): 4bpp chunky, high nibble first, pixels at +6.
    Types 0x10/0x12 (Amiga only): 5 whole bitplanes (32 colours), pixels at +6.
    Amiga scripts whose typepack has bit 0 clear (0x80/0xC0) store type 0/2 as 4 whole bitplanes."""
    t = s.data[at]
    w = s.u16(at + 2) + 1; h = s.u16(at + 4) + 1
    px = at + 6; d = s.data; n = w * h
    rows = []
    planar = t in (0x10, 0x12) or (not s.le and (s.typepack & 1) == 0)
    if planar:
        npl = 5 if t in (0x10, 0x12) else 4
        plane = n // 8
        for y in range(h):
            row = []
            for x in range(w):
                bit = y * w + x; o = px + (bit >> 3); sh = 7 - (bit & 7); v = 0
                for p in range(npl):
                    if (d[o + p * plane] >> sh) & 1: v |= 1 << p
                row.append(v)
            rows.append(row)
        return t, w, h, npl, rows
    for y in range(h):
        row = []
        for x in range(w):
            i = y * w + x; b = d[px + (i >> 1)] if px + (i >> 1) < len(d) else 0
            row.append(b >> 4 if (i & 1) == 0 else b & 15)
        rows.append(row)
    return t, w, h, 4, rows

def palette_rgb(s, at):
    """0xFE palette -> list of (r,g,b) 8-bit. Amiga: 12-bit; PC: 9-bit (ST style)."""
    n = s.data[at + 1]
    cnt = 16 if n == 0 else 32 if not s.le else n + 1
    out = []
    for c in range(cnt):
        hi = s.data[at + 2 + 2 * c]; lo = s.data[at + 3 + 2 * c]
        r, g, b = hi & 15, lo >> 4, lo & 15
        if s.le:  # 3 bits per gun
            out.append(((r & 7) * 255 // 7, (g & 7) * 255 // 7, (b & 7) * 255 // 7))
        else:
            out.append((r * 17, g * 17, b * 17))
    return out

def write_png(path, rows_rgb):
    import zlib, struct as st
    h = len(rows_rgb); w = len(rows_rgb[0])
    raw = b''.join(b'\0' + bytes(c for px in r for c in px) for r in rows_rgb)
    def chunk(t, d): return st.pack('>I', len(d)) + t + d + st.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', st.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))

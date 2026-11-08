"""Draws the app icon (USB-stick-with-Wi-Fi-waves) into app.ico. No dependencies."""
import math, struct, sys

def render(n):
    px = []
    ss = 4  # supersampling
    for y in range(n):
        row = []
        for x in range(n):
            acc = [0, 0, 0, 0]
            for sy in range(ss):
                for sx in range(ss):
                    u = (x + (sx + 0.5) / ss) / n
                    v = (y + (sy + 0.5) / ss) / n
                    c = sample(u, v)
                    a = c[3]
                    acc[0] += c[0] * a; acc[1] += c[1] * a; acc[2] += c[2] * a; acc[3] += a
            a = acc[3] / (ss * ss)
            if acc[3] > 0:
                r, g, b = acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3]
            else:
                r = g = b = 0
            row.append((int(r), int(g), int(b), int(a * 255)))
        px.append(row)
    return px

def rounded(u, v, x0, y0, x1, y1, rad):
    cx = min(max(u, x0 + rad), x1 - rad)
    cy = min(max(v, y0 + rad), y1 - rad)
    return (u - cx) ** 2 + (v - cy) ** 2 <= rad * rad and x0 <= u <= x1 and y0 <= v <= y1

def sample(u, v):
    # background tile
    if not rounded(u, v, 0.04, 0.04, 0.96, 0.96, 0.18):
        return (0, 0, 0, 0)
    col = (0, 110, 200, 1)
    # USB plug body at the bottom
    if rounded(u, v, 0.36, 0.62, 0.64, 0.90, 0.04):
        return (245, 245, 245, 1)
    if rounded(u, v, 0.41, 0.52, 0.59, 0.63, 0.02):
        return (200, 205, 212, 1)
    # Wi-Fi arcs above, centred on the plug top
    cx, cy = 0.5, 0.56
    d = math.hypot(u - cx, v - cy)
    ang = math.degrees(math.atan2(cy - v, u - cx))
    if 45 <= ang <= 135:
        for r0 in (0.12, 0.24, 0.36):
            if abs(d - r0) <= 0.035:
                return (255, 255, 255, 1)
    return col

def bmp_entry(n):
    px = render(n)
    hdr = struct.pack('<IiiHHIIiiII', 40, n, n * 2, 1, 32, 0, n * n * 4, 0, 0, 0, 0)
    data = b''.join(struct.pack('BBBB', p[2], p[1], p[0], p[3]) for row in reversed(px) for p in row)
    mask_row = ((n + 31) // 32) * 4
    return hdr + data + b'\x00' * (mask_row * n)

sizes = [16, 24, 32, 48, 64]
blobs = [bmp_entry(n) for n in sizes]
out = struct.pack('<HHH', 0, 1, len(sizes))
off = 6 + 16 * len(sizes)
for n, b in zip(sizes, blobs):
    out += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(b), off)
    off += len(b)
out += b''.join(blobs)
open(sys.argv[1], 'wb').write(out)
print('icon written', len(out), 'bytes')

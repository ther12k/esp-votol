#!/usr/bin/env python3
"""tft-dash wiring diagram v2: side-by-side shield<->board, jumpers, touch, free-pin plan."""
from PIL import Image, ImageDraw, ImageFont
import os

W, H = 1400, 1200
BG      = (14, 20, 32)
PCB     = (34, 46, 64)
SOCKET  = (12, 16, 24)
PCB_EDGE= (90, 110, 135)
SHIELD  = (150, 48, 44)
SCREEN  = (8, 12, 18)
TXT     = (225, 232, 242)
DIM     = (130, 142, 158)
ORANGE  = (255, 150, 40)
YELLOW  = (250, 210, 60)
GREEN   = (60, 220, 120)
CYAN    = (70, 190, 255)
MAGENTA = (240, 90, 220)
RED     = (255, 90, 80)

def font(sz, bold=False):
    p = f"/usr/share/fonts/truetype/dejavu/DejaVuSans{'-Bold' if bold else ''}.ttf"
    return ImageFont.truetype(p, sz) if os.path.exists(p) else ImageFont.load_default()

img = Image.new("RGB", (W, H), BG)
d = ImageDraw.Draw(img)
F  = lambda s: font(s); FB = lambda s: font(s, True)

def arrow(x0, y, x1, col, w=7):
    d.line([x0, y, x1 - 10, y], fill=col, width=w)
    d.polygon([(x1, y), (x1 - 16, y - 9), (x1 - 16, y + 9)], fill=col)
    d.ellipse([x0 - w/2, y - w/2, x0 + w/2, y + w/2], fill=col)

def dashline(x0, y, x1, col, w=4):
    x = x0
    while x < x1 - 8:
        d.line([x, y, min(x + 10, x1 - 8), y], fill=col, width=w)
        x += 20

def numcircle(x, y, n, col):
    d.ellipse([x - 16, y - 16, x + 16, y + 16], fill=col)
    d.text((x, y), str(n), font=FB(19), fill=(10, 12, 16), anchor="mm")

def socket(x, y, label, col=None, side="in", sz=18):
    d.rectangle([x - 12, y - 12, x + 12, y + 12], fill=SOCKET,
                outline=col or PCB_EDGE, width=3 if col else 2)
    if label:
        if side == "in":   # label toward board interior (left of socket)
            d.text((x - 18, y), label, font=FB(sz) if col else F(sz),
                   fill=col or DIM, anchor="rm")
        else:              # label outside (right of socket)
            d.text((x + 18, y), label, font=FB(sz) if col else F(sz),
                   fill=col or DIM, anchor="lm")

# ---------------- title ----------------
d.text((40, 22), "tft-dash wiring v2 — side-by-side + free-pin plan", font=FB(30), fill=TXT)
d.text((40, 62), "3 wires make the LCD work · shield still seats FULLY on the headers — the wires are extra, not instead",
       font=F(17), fill=DIM)

# ================================================= BOARD (left)
bx0, by0, bx1, by1 = 60, 110, 740, 1120
d.rounded_rectangle([bx0, by0, bx1, by1], 16, fill=PCB, outline=PCB_EDGE, width=3)
d.rounded_rectangle([350, 1120, 450, 1146], 4, fill=(160, 160, 170))
d.text((400, 1124), "USB", font=FB(14), fill=(20, 24, 30), anchor="mm")
d.rectangle([280, 830, 560, 1060], fill=(24, 30, 42), outline=PCB_EDGE)
d.text((420, 920), "ESP32", font=F(20), fill=DIM, anchor="mm")
d.text((420, 950), "WROOM-32", font=F(18), fill=DIM, anchor="mm")
d.text((420, 985), "(under the shield)", font=F(15), fill=DIM, anchor="mm")
d.text((400, 1092), "WEMOS D1 R32  ·  schematic view — verify each socket by its printed number",
       font=F(16), fill=(95, 108, 124), anchor="mm")

# left rail (analog positions, clone prints GPIO numbers)
lx = 140
left = [("IO2  (A0)", None), ("IO4  (A1)", None), ("IO35 (A2)", ORANGE),
        ("IO34 (A3)", YELLOW), ("IO36 (A4)", GREEN), ("IO39 (A5)", MAGENTA)]
d.text((lx, by0 - 18), "analog rail", font=FB(15), fill=DIM, anchor="mm")
for i, (lab, col) in enumerate(left):
    socket(lx, 190 + i * 46, lab, col, "in")

# right rail — grouped
rx = 660
groups = [
    ("LCD control (the 3 wires)", [
        ("IO15", ORANGE), ("IO33", YELLOW), ("GND", None), ("IO32", GREEN)]),
    ("free — spares", [
        ("IO18", MAGENTA), ("IO23", MAGENTA), ("IO19", MAGENTA),
        ("IO21", MAGENTA), ("IO5", MAGENTA), ("IO22", MAGENTA)]),
]
d.text((rx, by0 - 18), "digital rail", font=FB(15), fill=DIM, anchor="mm")
ry = 190
gy = {}
for gname, rows in groups:
    for lab, col in rows:
        gy[lab] = ry
        socket(rx, ry, lab, col, "in")
        ry += 46
    d.text((rx - 30, ry - 22), gname, font=F(14), fill=(95, 108, 124), anchor="rm")
    ry += 20

# ================================================= SHIELD (right)
sx0, sy0, sx1, sy1 = 900, 70, 1330, 640
d.rounded_rectangle([sx0, sy0, sx1, sy1], 10, fill=SHIELD, outline=(190, 80, 72), width=3)
d.rectangle([1000, 100, 1310, 360], fill=SCREEN, outline=(70, 30, 28))
d.text((1155, 200), "2.4\" TFT SHIELD", font=FB(22), fill=TXT, anchor="mm")
d.text((1155, 232), "mcufriend 8-bit parallel", font=F(16), fill=(255, 190, 185), anchor="mm")
d.text((1155, 275), "data D0-D7 + RD + WR", font=F(16), fill=(255, 190, 185), anchor="mm")
d.text((1155, 300), "go through the UNO sockets", font=F(16), fill=(255, 190, 185), anchor="mm")
d.text((1155, 325), "(no wires needed)", font=F(16), fill=(255, 190, 185), anchor="mm")

# shield sockets on left edge (facing the board) — A2/A3/A4 sit at the
# same height as the board socket their wire reaches, so wires are straight.
# Labels show the FUNCTION; positions A2-A4 are never printed on shields —
# they are identified by the IO35/34/36 sockets below them.
shx = 900
sh = [("A0-pos", None, 98), ("A1-pos", None, 144),
      ("LCD_RS\n(pos A2)", ORANGE, gy["IO15"]),
      ("LCD_CS\n(pos A3)", YELLOW, gy["IO33"]),
      ("LCD_RST\n(pos A4)", GREEN, gy["IO32"]),
      ("A5-pos", None, 374)]
for lab, col, y in sh:
    socket(shx, y, None, col)
    lines = lab.split("\n")
    for j, ln in enumerate(lines):
        yy = y + (j - (len(lines) - 1) / 2) * 17
        d.text((shx + 20, yy), ln, font=FB(17) if col else F(16),
               fill=col or DIM, anchor="lm")
tch = []
for lab, y in tch:
    socket(shx, y, None, CYAN)
    d.text((shx + 20, y), lab, font=F(16), fill=CYAN, anchor="lm")
for sl, ty in []:
    dashline(886, gy[ty], 678, CYAN)

# ---------------- the 3 LCD wires ----------------
wires = [("A2", gy["IO15"], ORANGE, "1"), ("A3", gy["IO33"], YELLOW, "2"), ("A4", gy["IO32"], GREEN, "3")]
for sl, ty, col, n in wires:
    y = ty
    arrow(886, y, 678, col)
    numcircle((886 + 678) // 2, y - 0, n, col)

d.text((1115, 668), "A0-A5 = positions, not labels — find them by the IO35/34/36 sockets below",
       font=F(14), fill=(250, 200, 120), anchor="mm")

# ---------------- touch facts box ----------------
d.rounded_rectangle([784, 505, 1320, 660], 12, outline=(250, 200, 120), width=3)
d.text((800, 528), "TOUCH on this shield", font=FB(20), fill=(250, 200, 120), anchor="lm")
for i, t in enumerate([
    "No touch chip, no T_ pins: the resistive film is wired",
    "straight to 4 UNO pins shared with the LCD (A1/A2/D6/D7).",
    "On the D1 R32 those are ADC2 channels - the ESP32 cannot",
    "analogRead them while WiFi is on  ->  touch disabled;",
    "pages auto-cycle. Future on-device control: physical",
    "buttons on the free pins instead.",
]):
    d.text((800, 560 + i * 17), t, font=F(15), fill=TXT, anchor="lm")

# ================================================= FREE-PIN PLAN panel
px0, py0, px1, py1 = 820, 700, 1340, 1000
d.rounded_rectangle([px0, py0, px1, py1], 12, outline=MAGENTA, width=3)
d.text(((px0 + px1) // 2, py0 + 30), "FREE PINS — 7 remain (touch unused)", font=FB(24), fill=MAGENTA, anchor="mm")
rows = [
    ("IO5",  "ALARM OUT 1", "buzzer / siren trigger"),
    ("IO22", "ALARM OUT 2", "relay or strobe light"),
    ("IO39", "INPUT", "vibration SW-420 / sense line (input-only pin)"),
]
for i, (pin, role, desc) in enumerate(rows):
    y = py0 + 80 + i * 74
    d.rounded_rectangle([px0 + 20, y, px0 + 130, y + 56], 8, fill=MAGENTA)
    d.text((px0 + 75, y + 28), pin, font=FB(24), fill=(10, 12, 16), anchor="mm")
    d.text((px0 + 150, y + 14), role, font=FB(20), fill=TXT, anchor="lm")
    d.text((px0 + 150, y + 40), desc, font=F(17), fill=DIM, anchor="lm")
d.text((px0 + 20, py0 + 300), "spare: IO18 · IO19 · IO21 · IO23", font=FB(18), fill=MAGENTA, anchor="lm")

# ---------------- taken / never panel
tx0, ty0, tx1, ty1 = 820, 1020, 1340, 1160
d.rounded_rectangle([tx0, ty0, tx1, ty1], 12, outline=RED, width=3)
d.text((tx0 + 20, ty0 + 26), "NEVER CONNECT anything to:", font=FB(20), fill=RED, anchor="lm")
for i, t in enumerate([
    "IO0 (boot) · IO1/IO3 (USB serial)",
    "GPIO 6-11 — the empty SD0/SD1/SD2/SD3/CMD/CLK holes = flash bus",
]):
    d.text((tx0 + 20, ty0 + 58 + i * 30), t, font=F(17), fill=TXT, anchor="lm")
d.text((tx0 + 20, ty0 + 122), "busy: LCD 12,13,26,25,17,16,27,14,2,4,15,33,32 · 35,34,36 tied to wires",
       font=F(15), fill=(95, 108, 124), anchor="lm")

out = os.path.expanduser("~/Workspace/Learning/esp-votol/tft-dash/wiring-diagram.png")
img.save(out)
print("saved", out)

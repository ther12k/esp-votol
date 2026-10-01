#!/usr/bin/env python3
"""Board-only jumper diagram: all 3 wires between points ON the D1 R32 itself."""
from PIL import Image, ImageDraw, ImageFont
import os

W, H = 1500, 1150
BG      = (14, 20, 32)
PCB     = (34, 46, 64)
SOCKET  = (12, 16, 24)
PCB_EDGE= (90, 110, 135)
SHIELD_OVER = (150, 48, 44)
TXT     = (225, 232, 242)
DIM     = (130, 142, 158)
ORANGE  = (255, 150, 40)
YELLOW  = (250, 210, 60)
GREEN   = (60, 220, 120)
MAGENTA = (240, 90, 220)

def font(sz, bold=False):
    p = f"/usr/share/fonts/truetype/dejavu/DejaVuSans{'-Bold' if bold else ''}.ttf"
    return ImageFont.truetype(p, sz) if os.path.exists(p) else ImageFont.load_default()

img = Image.new("RGB", (W, H), BG)
d = ImageDraw.Draw(img)
F  = lambda s: font(s); FB = lambda s: font(s, True)

def bez(p0, p1, p2, n=60):
    pts = []
    for i in range(n + 1):
        t = i / n; u = 1 - t
        pts.append((u*u*p0[0] + 2*u*t*p1[0] + t*t*p2[0],
                    u*u*p0[1] + 2*u*t*p1[1] + t*t*p2[1]))
    return pts

def thick_curve(pts, color, w=7):
    d.line(pts, fill=color, width=w, joint="curve")
    for e in (pts[0], pts[-1]):
        d.ellipse([e[0]-w/2, e[1]-w/2, e[0]+w/2, e[1]+w/2], fill=color)

def point(x, y, label, col=None, lx=None, anchor="lm", sz=17):
    d.rectangle([x - 11, y - 11, x + 11, y + 11], fill=SOCKET,
                outline=col or PCB_EDGE, width=3 if col else 2)
    if label:
        d.text((lx if lx is not None else x + 16, y), label,
               font=FB(sz) if col else F(sz), fill=col or DIM, anchor=anchor)

# ---------------- title ----------------
d.text((40, 20), "Board-only jumpers — wires stay ON the ESP32 board",
       font=FB(30), fill=TXT)
d.text((40, 60), "shield stays fully seated & untouched · its RS/CS/RST arrive through the pins already plugged into 35/34/36 · tap them at the free same-number points",
       font=F(17), fill=DIM)

# ================================================= BOARD
bx0, by0, bx1, by1 = 60, 120, 860, 1090
d.rounded_rectangle([bx0, by0, bx1, by1], 16, fill=PCB, outline=PCB_EDGE, width=3)

# ESP32 module (center)
d.rectangle([360, 470, 600, 700], fill=(24, 30, 42), outline=PCB_EDGE)
d.text((480, 560), "ESP32", font=F(22), fill=DIM, anchor="mm")
d.text((480, 592), "WROOM-32", font=F(18), fill=DIM, anchor="mm")

# ---- OUTER rails (UNO positions) — covered by the shield
d.rounded_rectangle([90, 170, 340, 1040], 10, outline=SHIELD_OVER, width=3)
d.rounded_rectangle([610, 170, 840, 1040], 10, outline=SHIELD_OVER, width=3)
d.text((215, 1055), "outer UNO rail — COVERED by shield", font=F(14), fill=(230, 140, 130), anchor="mm")
d.text((725, 1055), "outer UNO rail — COVERED", font=F(14), fill=(230, 140, 130), anchor="mm")

olx = 130   # outer-left x
for i, (lab, col) in enumerate([("IO2", None), ("IO4", None), ("IO35", ORANGE),
                                 ("IO34", YELLOW), ("IO36", GREEN), ("IO39", None)]):
    point(olx, 210 + i * 44, lab, col, lx=olx - 18, anchor="rm")
d.text((olx + 40, 210 + 2*44 - 52), "shield pin inside:", font=F(13), fill=(230, 140, 130), anchor="lm")
d.text((olx + 40, 210 + 2*44 - 38), "RS / CS / RST enter", font=F(13), fill=(230, 140, 130), anchor="lm")
d.text((olx + 40, 210 + 2*44 - 24), "the board here", font=F(13), fill=(230, 140, 130), anchor="lm")

orx = 800   # outer-right x
for i, lab in enumerate(["IO26", "IO25", "IO17", "IO16", "IO27", "IO14", "IO12", "IO13"]):
    point(orx, 210 + i * 44, lab, None, lx=orx + 18, anchor="lm", sz=15)

# ---- INNER rails — FREE (next to the module)
ilx = 280   # inner-left x
d.text((ilx, 155), "inner rail — FREE", font=FB(15), fill=(120, 200, 160), anchor="mm")
inner_left = [("IO36", GREEN), ("IO39", None), ("IO34", YELLOW), ("IO35", ORANGE),
              ("IO32", GREEN), ("IO33", YELLOW)]
Lpos = {}
for i, (lab, col) in enumerate(inner_left):
    y = 210 + i * 44
    Lpos[lab] = y
    point(ilx, y, lab, col, lx=ilx - 18, anchor="rm")

irx = 660   # inner-right x
d.text((irx, 155), "inner rail — FREE", font=FB(15), fill=(120, 200, 160), anchor="mm")
inner_right = ["IO13", "GND", "IO15", "IO2", "IO0", "IO4", "IO16", "IO17"]
Rpos = {}
for i, lab in enumerate(inner_right):
    y = 210 + i * 44
    Rpos[lab] = y
    point(irx, y, lab, ORANGE if lab == "IO15" else None, lx=irx + 18, anchor="lm")

# ---- the 3 wires ON the board
# 35 -> 15 : long arc over the module
thick_curve(bez((ilx, Lpos["IO35"]), (480, 380), (irx, Rpos["IO15"])), ORANGE)
d.ellipse([466, 372, 494, 400], fill=ORANGE)
d.text((480, 386), "1", font=FB(19), fill=(10, 12, 16), anchor="mm")
# 34 -> 33 : short arc, both on inner-left
thick_curve(bez((ilx - 12, Lpos["IO34"]), (ilx - 90, (Lpos["IO34"] + Lpos["IO33"]) / 2),
                (ilx - 12, Lpos["IO33"])), YELLOW)
d.text((ilx - 105, (Lpos["IO34"] + Lpos["IO33"]) / 2), "2", font=FB(19), fill=YELLOW, anchor="mm")
# 36 -> 32 : short arc, both on inner-left
thick_curve(bez((ilx - 12, Lpos["IO36"]), (ilx - 130, (Lpos["IO36"] + Lpos["IO32"]) / 2),
                (ilx - 12, Lpos["IO32"])), GREEN)
d.text((ilx - 148, (Lpos["IO36"] + Lpos["IO32"]) / 2), "3", font=FB(19), fill=GREEN, anchor="mm")

# note under board
d.text((460, 1118), "schematic top view — find each point by the number printed beside it",
       font=F(15), fill=(95, 108, 124), anchor="mm")

# ================================================= RIGHT PANELS
px = 920

d.text((px, 150), "THE 3 WIRES (female–female)", font=FB(24), fill=TXT)
rows = [("1", "IO35", "IO15", ORANGE, "LCD_RS"),
        ("2", "IO34", "IO33", YELLOW, "LCD_CS"),
        ("3", "IO36", "IO32", GREEN,  "LCD_RST")]
for i, (n, a, b, col, fn) in enumerate(rows):
    y = 200 + i * 64
    d.rounded_rectangle([px, y, px + 500, y + 52], 10, fill=(20, 28, 42), outline=col, width=2)
    d.ellipse([px + 12, y + 12, px + 42, y + 42], fill=col)
    d.text((px + 27, y + 27), n, font=FB(18), fill=(10, 12, 16), anchor="mm")
    d.text((px + 56, y + 27), a, font=FB(21), fill=col, anchor="lm")
    d.text((px + 140, y + 27), "──►", font=FB(20), fill=col, anchor="lm")
    d.text((px + 205, y + 27), b, font=FB(21), fill=col, anchor="lm")
    d.text((px + 290, y + 27), fn, font=F(18), fill=DIM, anchor="lm")

d.rounded_rectangle([px - 16, 425, px + 540, 640], 12, outline=(120, 200, 160), width=3)
d.text((px, 450), "WHY this works — no shield contact", font=FB(20), fill=(120, 200, 160), anchor="lm")
for i, t in enumerate([
    "1. shield pins plug into the covered outer sockets 35/34/36,",
    "   so RS/CS/RST signals enter the board there;",
    "2. inside the PCB, every socket with the same GPIO number",
    "   is one net — the free inner 35/34/36 carry the same signal;",
    "3. the wire hands it to 15/33/32, which the firmware drives.",
]):
    d.text((px, 485 + i * 27), t, font=F(17), fill=TXT if not t.startswith("   ") else (180, 190, 200), anchor="lm")

d.rounded_rectangle([px - 16, 665, px + 540, 830], 12, outline=(250, 200, 120), width=3)
d.text((px, 690), "Connection points on YOUR clone", font=FB(20), fill=(250, 200, 120), anchor="lm")
for i, t in enumerate([
    "• point is a male pin → push the female jumper onto it",
    "• point is an open socket → solid wire strand instead",
    "• no free duplicate 35/34/36? → back-side solder pads:",
    "  pad-35↔15, pad-34↔33, pad-36↔32 (always works)",
]):
    d.text((px, 725 + i * 26), t, font=F(17), fill=TXT, anchor="lm")

d.rounded_rectangle([px - 16, 855, px + 540, 955], 12, outline=MAGENTA, width=3)
d.text((px, 878), "Meanwhile the 3 free pins keep their jobs:", font=FB(19), fill=MAGENTA, anchor="lm")
for i, t in enumerate(["IO5 → alarm out 1 · IO22 → alarm out 2 · IO39 → input"]):
    d.text((px, 912 + i * 26), t, font=F(18), fill=TXT, anchor="lm")

out = os.path.expanduser("~/Workspace/Learning/esp-votol/tft-dash/wiring-board-jumpers.png")
img.save(out)
print("saved", out)

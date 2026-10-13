"""FoundryFSDK - make_icons.py: the UI glyph kit for the social/chat sample UMG widgets
(Pillow, deterministic).

    python make_icons.py

Writes every PNG next to Content/UI/Icons/src (relative to this script) plus a review
sheet (_contact_sheet.png) and prints one manifest line per file. Same input -> same
bytes, every run.

Ported from GooCrew/SourceAssets/icons/make_icons.py (same drawing primitives, same
white-glyph-on-transparent convention) - read that file before changing this one.

CONVENTIONS (match the Goo Crew UI kit - see umg-ui-conventions.md in the platform repo):
  - 256x256, white glyphs on transparent. TINT in UMG (Image -> Color and Opacity).
  - Supersampled 4x then LANCZOS-downsampled for anti-aliasing.
  - Strokes are >= 9% of the canvas so they hold at 18-28 px in a chat/roster list.
  - Import as UserInterface2D (RGBA) / UI texture group / NoMipmaps / sRGB on. These
    are SOURCE PNGs only - a later commandlet imports them into the plugin content;
    do not try to import into Unreal from this script.

Adding a glyph = one draw_<name>(d, s) function (canvas fractions, s = supersampled
size) plus one line in the GLYPHS table at the bottom of this file.
"""

import os
from math import cos, pi, radians, sin, sqrt

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.normpath(os.path.join(HERE, "..", "..", "Content", "UI", "Icons", "src"))
SIZE = 256
SS = 4  # supersample for anti-aliasing

# Contact-sheet background (Deep Sapphire Pulse --surface token, #0c1324).
BG = (0x0C, 0x13, 0x24)
ACCENT = (0x00, 0xDA, 0xF3)  # --primary cyan, for the sheet title
CREAM = (220, 225, 251)  # --on-surface

WHITE = (255, 255, 255, 255)
CLEAR = (0, 0, 0, 0)

WRITTEN = []  # (name, w, h) in write order, feeds the manifest + contact sheet


def _rgba(c, a=255):
    return (c[0], c[1], c[2], a)


def _out(name):
    return os.path.join(OUT_DIR, name)


def glyph(name, draw_fn):
    big = SIZE * SS
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    draw_fn(d, big)
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    img.save(_out(name))
    WRITTEN.append((name, SIZE, SIZE))


# --------------------------------------------------------------------------------------
# Glyph helpers. Every coordinate is a fraction of the canvas; s is the supersampled size.
# Drawing CLEAR replaces pixels (no blending), which is how holes and gaps are cut.
# Copied verbatim (same behavior) from GooCrew/SourceAssets/icons/make_icons.py so this
# file stays self-contained.
# --------------------------------------------------------------------------------------


def _F(s, pts):
    return [(x * s, y * s) for x, y in pts]


def _line(d, s, pts, w, fill=WHITE, caps=True):
    """Polyline with round joints and (by default) round caps."""
    P = _F(s, pts)
    width = max(1, int(round(w * s)))
    d.line(P, fill=fill, width=width, joint="curve")
    if caps:
        r = width / 2.0
        for x, y in (P[0], P[-1]):
            d.ellipse([x - r, y - r, x + r, y + r], fill=fill)


def _dot(d, s, cx, cy, r, fill=WHITE):
    d.ellipse([(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s], fill=fill)


def _ring(d, s, cx, cy, r, w, fill=WHITE, a0=None, a1=None):
    box = [(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s]
    width = max(1, int(round(w * s)))
    if a0 is None:
        d.ellipse(box, outline=fill, width=width)
    else:
        d.arc(box, a0, a1, fill=fill, width=width)


def _poly(d, s, pts, fill=WHITE, round_w=0):
    """Filled polygon; round_w > 0 rounds every corner outward by round_w / 2."""
    P = _F(s, pts)
    d.polygon(P, fill=fill)
    if round_w:
        width = max(1, int(round(round_w * s)))
        d.line(P + P[:2], fill=fill, width=width, joint="curve")
        r = width / 2.0
        for x, y in P:
            d.ellipse([x - r, y - r, x + r, y + r], fill=fill)


def _rrect(d, s, box, r, fill=WHITE, outline=None, w=0):
    B = [box[0] * s, box[1] * s, box[2] * s, box[3] * s]
    width = max(1, int(round(w * s))) if w else 1
    d.rounded_rectangle(B, radius=r * s, fill=fill, outline=outline, width=width)


def _rot(pts, cx, cy, deg):
    a = radians(deg)
    c, sn = cos(a), sin(a)
    return [(cx + (x - cx) * c - (y - cy) * sn, cy + (x - cx) * sn + (y - cy) * c) for x, y in pts]


def _person(d, s, cx, top, k, fill=WHITE):
    d.ellipse([(cx - 0.16 * k) * s, top * s, (cx + 0.16 * k) * s, (top + 0.32 * k) * s], fill=fill)
    d.pieslice([(cx - 0.34 * k) * s, (top + 0.38 * k) * s, (cx + 0.34 * k) * s, (top + 1.06 * k) * s], 180, 360, fill=fill)


def _slash(d, s, p0=(0.16, 0.16), p1=(0.84, 0.84), w=0.10, gap=0.09):
    _line(d, s, [p0, p1], w + gap, CLEAR)
    _line(d, s, [p0, p1], w, WHITE)


# --------------------------------------------------------------------------------------
# FSDK-specific helpers: a bottom-right corner badge (gap-cut so it reads as a distinct
# emblem over whatever glyph it sits on) and a simple question-mark mark for it.
# --------------------------------------------------------------------------------------


def _badge_gap(d, s, cx, cy, r):
    """Cut a clear disc so a badge drawn on top never fuses with the glyph underneath."""
    _dot(d, s, cx, cy, r, CLEAR)


def _badge_plus(d, s, cx, cy, r, fill=WHITE):
    arm = r * 0.62
    w = r * 0.46
    _line(d, s, [(cx - arm, cy), (cx + arm, cy)], w, fill, caps=False)
    _line(d, s, [(cx, cy - arm), (cx, cy + arm)], w, fill, caps=False)


def _badge_minus(d, s, cx, cy, r, fill=WHITE):
    arm = r * 0.62
    w = r * 0.46
    _line(d, s, [(cx - arm, cy), (cx + arm, cy)], w, fill, caps=False)


def _arc_pts(cx, cy, r, bearing0, bearing1, n=22):
    """Sample points on a circle; bearing 0 = straight up, increasing clockwise (compass-style,
    easier to reason about than PIL's 3-o'clock-zero arc angles for a hand-built hook)."""
    pts = []
    for i in range(n + 1):
        t = i / float(n)
        b = radians(bearing0 + (bearing1 - bearing0) * t)
        pts.append((cx + r * sin(b), cy - r * cos(b)))
    return pts


def _badge_qmark(d, s, cx, cy, r, fill=WHITE):
    """A question mark built as one smooth stroked curve (round caps/joints, no flat arc-end
    artifacts) - a hook from upper-left, over the top, down the right side, into a short
    stem - plus a separated dot."""
    w = r * 0.30
    hook_cx, hook_cy = cx, cy - r * 0.28
    hook_r = r * 0.46
    hook = _arc_pts(hook_cx, hook_cy, hook_r, -35, 175, 22)
    stem_end = (cx, cy + r * 0.10)
    _line(d, s, hook + [stem_end], w, fill, caps=True)
    _dot(d, s, cx, cy + r * 0.58, w * 0.58, fill)


# --------------------------------------------------------------------------------------
# Glyphs carried over unchanged from the Goo Crew kit (same drawing code).
# --------------------------------------------------------------------------------------


def draw_close(d, s):
    # Smaller cross than the other glyphs on purpose (Justin, 2026-10-11: "the X icon all around the game is a bit too big").
    w = s * 0.08
    m = s * 0.30
    d.line([(m, m), (s - m, s - m)], fill=(255, 255, 255, 255), width=int(w))
    d.line([(s - m, m), (m, s - m)], fill=(255, 255, 255, 255), width=int(w))


def draw_search(d, s):
    _ring(d, s, 0.42, 0.42, 0.27, 0.10)
    _line(d, s, [(0.63, 0.63), (0.86, 0.86)], 0.12)


def draw_chevron_up(d, s):
    _line(d, s, [(0.18, 0.66), (0.50, 0.34), (0.82, 0.66)], 0.11)


def draw_chevron_down(d, s):
    _line(d, s, [(0.18, 0.34), (0.50, 0.66), (0.82, 0.34)], 0.11)


def draw_check(d, s):
    _line(d, s, [(0.16, 0.52), (0.40, 0.76), (0.84, 0.28)], 0.12)


def draw_person(d, s):
    white = (255, 255, 255, 255)
    d.ellipse([s * 0.34, s * 0.12, s * 0.66, s * 0.44], fill=white)
    d.pieslice([s * 0.16, s * 0.50, s * 0.84, s * 1.18], 180, 360, fill=white)


def draw_users(d, s):
    _person(d, s, 0.64, 0.16, 0.85)
    _person(d, s, 0.38, 0.20, 0.90, CLEAR)
    _person(d, s, 0.38, 0.24, 0.82)


# --------------------------------------------------------------------------------------
# New FSDK glyphs: send, person/group badges, block.
# --------------------------------------------------------------------------------------


def draw_send(d, s):
    """Plain filled right-pointing arrow (a triangular head + a stem, one solid polygon) -
    Justin: the paper plane "has this weird streak through it" (the fold-crease cut), and a
    straight-up filled arrow reads cleaner at small sizes anyway."""
    x0, x1 = 0.10, 0.90  # ~80% of the canvas, centered
    head_x = 0.58
    hh = 0.30  # head half-height
    sh = 0.14  # stem half-height
    cy = 0.5
    pts = [
        (x0, cy - sh),
        (head_x, cy - sh),
        (head_x, cy - hh),
        (x1, cy),
        (head_x, cy + hh),
        (head_x, cy + sh),
        (x0, cy + sh),
    ]
    _poly(d, s, pts)


def draw_person_plus(d, s):
    """Same full-size figure as draw_person (Justin: the old 0.76-scale, off-centre figure "looks
    squeezed") - the plus badge sits bottom-right over the body; _badge_gap cuts a clean hole
    first so the badge reads as its own emblem rather than fusing with the figure under it."""
    _person(d, s, 0.5, 0.12, 1.0)
    bx, by, br = 0.78, 0.80, 0.20
    _badge_gap(d, s, bx, by, br)
    _badge_plus(d, s, bx, by, br)


def draw_person_minus(d, s):
    # Same full-size figure + badge geometry as draw_person_plus (one Remove / Add pair).
    _person(d, s, 0.5, 0.12, 1.0)
    bx, by, br = 0.78, 0.80, 0.20
    _badge_gap(d, s, bx, by, br)
    _badge_minus(d, s, bx, by, br)


def draw_block(d, s):
    _ring(d, s, 0.5, 0.5, 0.40, 0.11)
    _slash(d, s, w=0.11, gap=0.10)


def draw_group_plus(d, s):
    _person(d, s, 0.52, 0.06, 0.62)
    _person(d, s, 0.28, 0.10, 0.66, CLEAR)
    _person(d, s, 0.28, 0.13, 0.60)
    bx, by, br = 0.78, 0.78, 0.21
    _badge_gap(d, s, bx, by, br)
    _badge_plus(d, s, bx, by, br)


def draw_group_question(d, s):
    _person(d, s, 0.52, 0.06, 0.62)
    _person(d, s, 0.28, 0.10, 0.66, CLEAR)
    _person(d, s, 0.28, 0.13, 0.60)
    bx, by, br = 0.78, 0.78, 0.21
    _badge_gap(d, s, bx, by, br)
    _badge_qmark(d, s, bx, by, br)


def draw_crown(d, s):
    """Three-point crown with a base band - the party-leader mark (lobby nameplate CrownIcon,
    matches the drawer's HostBadge). Same drawing code as GooCrew/SourceAssets/icons/make_icons.py's
    draw_crown (ported, not reinvented)."""
    _poly(d, s, [(0.08, 0.28), (0.30, 0.50), (0.50, 0.18), (0.70, 0.50), (0.92, 0.28), (0.84, 0.72), (0.16, 0.72)], round_w=0.04)
    for x, y in ((0.08, 0.28), (0.50, 0.18), (0.92, 0.28)):
        _dot(d, s, x, y, 0.065)
    _rrect(d, s, (0.16, 0.77, 0.84, 0.88), 0.03)


def draw_external(d, s):
    """Open-external-link glyph (Feather-style): a box open at its top-right corner, with a small
    arrow breaking out through the gap toward the top-right - the usual "opens elsewhere" mark."""
    w = 0.10
    _line(d, s, [(0.75, 0.5417), (0.75, 0.7917), (0.2083, 0.7917), (0.2083, 0.25), (0.4583, 0.25)], w)
    _line(d, s, [(0.625, 0.125), (0.875, 0.125), (0.875, 0.375)], w)
    _line(d, s, [(0.4167, 0.5833), (0.875, 0.125)], w)


def draw_gear(d, s):
    """Eight-tooth gear with a round hub hole - the app bar's Settings button. Ported from
    GooCrew/SourceAssets/icons/make_icons.py's draw_gear (same geometry)."""
    n = 8
    ro, ri = 0.44, 0.33
    pts = []
    for i in range(n):
        a = 2 * pi * i / n
        for rr, off in ((ri, -0.30), (ro, -0.16), (ro, 0.16), (ri, 0.30)):
            pts.append((0.5 + rr * cos(a + off), 0.5 + rr * sin(a + off)))
    _poly(d, s, pts, round_w=0.04)
    _dot(d, s, 0.5, 0.5, 0.14, CLEAR)


def draw_spinner(d, s):
    """Open ring arc (a gap at the upper-left) - the match cluster's busy indicator, rotated at
    runtime. Ported from GooCrew/SourceAssets/icons/make_icons.py's draw_spinner (same geometry)."""
    w = s * 0.10
    m = s * 0.14
    d.arc([m, m, s - m, s - m], 300, 220, fill=(255, 255, 255, 255), width=int(w))


# --------------------------------------------------------------------------------------
# Contact sheet
# --------------------------------------------------------------------------------------


def contact_sheet(items, out_name, title):
    font = ImageFont.load_default(size=13)
    hfont = ImageFont.load_default(size=20)
    cols, cell_w, cell_h, thumb, head_h, pad = 6, 200, 196, 148, 44, 20
    rows = (len(items) + cols - 1) // cols
    height = pad + head_h + rows * cell_h + pad
    width = pad * 2 + cols * cell_w
    sheet = Image.new("RGBA", (width, height), _rgba(BG))
    d = ImageDraw.Draw(sheet)
    y = pad
    d.text((pad, y + 10), title, fill=_rgba(ACCENT), font=hfont)
    y += head_h
    for i, (name, w, h) in enumerate(items):
        cx = pad + (i % cols) * cell_w
        cy = y + (i // cols) * cell_h
        bx, by = cx + (cell_w - thumb) // 2, cy + 8
        for yy in range(0, thumb, 16):
            for xx in range(0, thumb, 16):
                tone = (24, 32, 52) if ((xx + yy) // 16) % 2 == 0 else (16, 22, 38)
                d.rectangle([bx + xx, by + yy, bx + xx + 15, by + yy + 15], fill=_rgba(tone))
        im = Image.open(_out(name)).convert("RGBA")
        k = min(thumb / float(w), thumb / float(h), 1.0)
        tw, th = max(1, int(round(w * k))), max(1, int(round(h * k)))
        im = im.resize((tw, th), Image.LANCZOS)
        sheet.alpha_composite(im, (bx + (thumb - tw) // 2, by + (thumb - th) // 2))
        label = name[:-4]
        tl = d.textlength(label, font=font)
        d.text((cx + (cell_w - tl) / 2, by + thumb + 8), label, fill=_rgba(CREAM), font=font)
    sheet.save(_out(out_name))
    return sheet.size


def contact_sheet_64(items, out_name, title):
    """Same glyphs at 64px over the dark surface tone, to prove legibility at list/button size."""
    font = ImageFont.load_default(size=12)
    hfont = ImageFont.load_default(size=18)
    cols, cell_w, cell_h, thumb, head_h, pad = 8, 100, 112, 64, 40, 20
    rows = (len(items) + cols - 1) // cols
    height = pad + head_h + rows * cell_h + pad
    width = pad * 2 + cols * cell_w
    sheet = Image.new("RGBA", (width, height), _rgba(BG))
    d = ImageDraw.Draw(sheet)
    y = pad
    d.text((pad, y + 8), title, fill=_rgba(ACCENT), font=hfont)
    y += head_h
    for i, (name, w, h) in enumerate(items):
        cx = pad + (i % cols) * cell_w
        cy = y + (i // cols) * cell_h
        bx, by = cx + (cell_w - thumb) // 2, cy
        im = Image.open(_out(name)).convert("RGBA")
        im = im.resize((thumb, thumb), Image.LANCZOS)
        sheet.alpha_composite(im, (bx, by))
        label = name[5:-4] if name.startswith("icon_") else name[:-4]
        tl = d.textlength(label, font=font)
        tl_x = cx + (cell_w - tl) / 2
        d.text((tl_x, by + thumb + 4), label, fill=_rgba(CREAM), font=font)
    sheet.save(_out(out_name))
    return sheet.size


# --------------------------------------------------------------------------------------
# Table. Add a glyph = one (name, draw_fn) line.
# --------------------------------------------------------------------------------------

GLYPHS = [
    ("icon_send.png", draw_send),
    ("icon_person_plus.png", draw_person_plus),
    ("icon_person_minus.png", draw_person_minus),
    ("icon_block.png", draw_block),
    ("icon_group_plus.png", draw_group_plus),
    ("icon_group_question.png", draw_group_question),
    ("icon_users.png", draw_users),
    ("icon_person.png", draw_person),
    ("icon_close.png", draw_close),
    ("icon_search.png", draw_search),
    ("icon_chevron_down.png", draw_chevron_down),
    ("icon_chevron_up.png", draw_chevron_up),
    ("icon_check.png", draw_check),
    ("icon_external.png", draw_external),
    ("icon_crown.png", draw_crown),
    ("icon_gear.png", draw_gear),
    ("icon_spinner.png", draw_spinner),
]

if __name__ == "__main__":
    os.makedirs(OUT_DIR, exist_ok=True)
    for name, fn in GLYPHS:
        glyph(name, fn)
    sw, sh = contact_sheet(list(WRITTEN), "_contact_sheet.png", "FoundryFSDK UI icons - white glyphs, tint in UMG")
    sw2, sh2 = contact_sheet_64(list(WRITTEN), "_contact_sheet_64.png", "same glyphs at 64px")
    for name, w, h in WRITTEN:
        print("UI %s %dx%d" % (name, w, h))
    print("UI _contact_sheet.png %dx%d (review sheet, do not import)" % (sw, sh))
    print("UI _contact_sheet_64.png %dx%d (legibility check at list/button size, do not import)" % (sw2, sh2))
    print("wrote %d PNGs + 2 contact sheets to %s" % (len(WRITTEN), OUT_DIR))

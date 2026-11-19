#!/usr/bin/env python3
"""Generate the aux-sniffer wiring schematics (SVG, light and dark themes).

    python3 -m pip install -r tools/schematic/requirements.txt
    python3 tools/schematic/schematic.py          # writes docs/schematic/*.svg

Every pin number and value here must match README.md, SPEC.md and
firmware/aux_sniffer/config.h. Pin numbers follow the README convention:
40-pin connector pin, with the 30-pin panel pin in parentheses.
"""

import re
from pathlib import Path

import schemdraw
import schemdraw.elements as elm

OUT = Path(__file__).resolve().parents[2] / "docs" / "schematic"

THEMES = {
    # GitHub light / dark palettes (Primer): fg, bg, accent (modifications), muted notes
    "light": dict(fg="#1f2328", bg="#ffffff", accent="#cf222e", muted="#59636e", box="#f6f8fa"),
    "dark": dict(fg="#e6edf3", bg="#0d1117", accent="#ff7b72", muted="#9198a1", box="#151b23"),
}

# Firmware pins (config.h, ESP32-C3 SuperMini build)
PIN_AUX_IN = 4
PIN_BL = 5
PIN_PANEL_VCC = 10
PIN_LED = 8

FS = 11  # base font size
FS_SMALL = 9


def text(d, xy, s, c, size=FS_SMALL, halign="left", valign="center"):
    d.add(elm.Label().at(xy).label(s, color=c, fontsize=size, halign=halign, valign=valign))


def cut(d, x, y, c):
    """Red X marking a cut PCB trace."""
    s = 0.3
    d.add(elm.Line().at((x - s, y - s)).to((x + s, y + s)).color(c).linewidth(2.2))
    d.add(elm.Line().at((x - s, y + s)).to((x + s, y - s)).color(c).linewidth(2.2))


def rail(d, xy, t, up=True):
    """Adapter 3.3 V rail symbol."""
    v = elm.Vdd().at(xy).label("3V3_ADP", fontsize=FS_SMALL)
    if not up:
        v = v.down()
    d.add(v)


def box(d, x0, y0, x1, y1, t, title, lines=()):
    d.add(elm.Rect(corner1=(x0, y0), corner2=(x1, y1)).color(t["fg"]).fill(t["box"]).zorder(0))
    text(d, ((x0 + x1) / 2, y0 - 0.45), title, t["fg"], FS, halign="center", valign="top")
    for i, s in enumerate(lines):
        text(d, ((x0 + x1) / 2, y0 - 1.15 - 0.55 * i), s, t["muted"], FS_SMALL, halign="center", valign="top")


def route(d, *pts):
    """Polyline through the given points."""
    for a, b in zip(pts, pts[1:]):
        d.add(elm.Line().at(a).to(b))


def installed(theme):
    """Full installed circuit: AUX tap, backlight (direct open-drain), panel VCC and HPD."""
    t = THEMES[theme]
    fg, acc, mut = t["fg"], t["accent"], t["muted"]
    d = schemdraw.Drawing(show=False)
    d.config(color=fg, bgcolor=t["bg"], fontsize=FS, font="sans-serif", lw=1.4, unit=2.5)

    # ---- rows: adapter traces running to the 40-pin connector
    Y_VCC, Y_HPD, Y_BL, Y_AUXP, Y_AUXN = 0.0, -4.2, -8.4, -11.4, -12.6
    XA, XC = 0.0, 25.0  # adapter pin column, connector pin column
    Y_BOX = -13.8

    box(d, XA - 6.0, 2.8, XA, Y_BOX, t, "Adapter", ["USB-C / DP to eDP", "(source side)"])
    box(d, XC, 2.8, XC + 6.6, Y_BOX, t, "40-pin connector", ["pin (panel pin)", "to LG LP156WF6-SPB1"])
    rows = [
        (Y_VCC, "3.3 V (panel VCC)", "18-21 (12-13) VCC"),
        (Y_HPD, "HPD in", "27 (17) HPD"),
        (Y_BL, "BL_EN out", "32 (22) BL_EN"),
        (Y_AUXP, "AUX+", "15 (9) AUX+"),
        (Y_AUXN, "AUX-", "16 (10) AUX-"),
    ]
    for y, a, c in rows:
        text(d, (XA - 0.25, y), a, fg, FS_SMALL, halign="right")
        text(d, (XC + 0.25, y), c, fg, FS_SMALL, halign="left")
        d.add(elm.Dot(open=True).at((XA, y)))
        d.add(elm.Dot(open=True).at((XC, y)))

    # ---- panel VCC switch: FDC606P bridging the cut VCC trace
    x_rail = 3.0
    d.add(elm.Line().at((XA, Y_VCC)).to((x_rail, Y_VCC)))
    d.add(elm.Dot().at((x_rail, Y_VCC)))
    d.add(elm.Line().at((x_rail, Y_VCC)).up(0.6))
    rail(d, (x_rail, Y_VCC + 0.6), t)
    x_s = 9.0
    d.add(elm.Line().at((x_rail, Y_VCC)).to((x_s, Y_VCC)))
    q = d.add(elm.PFet(bulk=False).flip().theta(-90).at((x_s, Y_VCC)))
    xg, yg = q.gate
    text(d, (xg, Y_VCC + 1.1), "FDC606P (P-MOSFET)", fg, FS_SMALL, halign="center", valign="bottom")
    text(d, (x_s - 0.15, Y_VCC + 0.2), "S (4)", mut, FS_SMALL, halign="right", valign="bottom")
    text(d, (q.drain[0] + 0.15, Y_VCC + 0.2), "D (1,2,5,6)", mut, FS_SMALL, halign="left", valign="bottom")
    text(d, (xg - 0.2, yg - 0.1), "G (3)", mut, FS_SMALL, halign="right", valign="top")
    d.add(elm.Line().at(q.drain).to((XC, Y_VCC)))
    text(d, (18.0, Y_VCC + 0.3), "original VCC trace cut,\nFDC606P bridges the cut", acc, FS_SMALL,
         halign="center", valign="bottom")

    # gate: 10 kOhm pull-down to GND, then down to GPIO10
    y_pd = Y_VCC - 2.6
    d.add(elm.Line().at(q.gate).to((xg, y_pd)))
    d.add(elm.Dot().at((xg, y_pd)))
    d.add(elm.Resistor().at((xg, y_pd)).right(2.2).label("10 kΩ", fontsize=FS_SMALL, loc="top"))
    d.add(elm.Ground().at((xg + 2.2, y_pd)))
    x_gate = xg

    # ---- HPD: source side pulled up, panel side floating
    x_hpu = 5.5
    d.add(elm.Line().at((XA, Y_HPD)).to((7.0, Y_HPD)))
    d.add(elm.Dot().at((x_hpu, Y_HPD)))
    d.add(elm.Resistor().at((x_hpu, Y_HPD)).up(2.0).label("10 kΩ", fontsize=FS_SMALL, loc="bottom"))
    rail(d, (x_hpu, Y_HPD + 2.0), t)
    cut(d, 7.5, Y_HPD, acc)
    d.add(elm.Line().at((8.0, Y_HPD)).to((XC, Y_HPD)).linestyle("--").color(mut))
    text(d, (18.0, Y_HPD + 0.3), "panel side left floating", mut, FS_SMALL, halign="center", valign="bottom")

    # ---- BL_EN: trace cut, 10 kOhm pull-up on the panel side, GPIO5 open-drain
    x_bl = 20.0
    d.add(elm.Line().at((XA, Y_BL)).to((7.0, Y_BL)).linestyle("--").color(mut))
    text(d, (3.5, Y_BL + 0.3), "no longer drives BL_EN", mut, FS_SMALL, halign="center", valign="bottom")
    cut(d, 7.5, Y_BL, acc)
    d.add(elm.Line().at((8.0, Y_BL)).to((XC, Y_BL)))
    d.add(elm.Dot().at((x_bl, Y_BL)))
    d.add(elm.Resistor().at((x_bl, Y_BL)).up(2.0).label("10 kΩ", fontsize=FS_SMALL, loc="bottom"))
    rail(d, (x_bl, Y_BL + 2.0), t)
    text(d, (x_bl + 2.4, Y_BL + 1.0), "external pull-up,\npanel side", mut, FS_SMALL)

    # ---- AUX taps: 1 kOhm + 10 nF in series into the MAX485
    x_ap, x_an = 1.4, 3.8
    for y in (Y_AUXP, Y_AUXN):
        d.add(elm.Line().at((XA, y)).to((XC, y)))
    d.add(elm.Dot().at((x_ap, Y_AUXP)))
    d.add(elm.Dot().at((x_an, Y_AUXN)))
    y0 = Y_BOX - 0.6
    d.add(elm.Line().at((x_ap, Y_AUXP)).to((x_ap, y0)))
    d.add(elm.Resistor().at((x_ap, y0)).down(2.2).label("1 kΩ", fontsize=FS_SMALL, loc="top"))
    d.add(elm.Capacitor().down(1.6).label("10 nF", fontsize=FS_SMALL, loc="top"))
    d.add(elm.Line().at((x_an, Y_AUXN)).to((x_an, y0)))
    d.add(elm.Resistor().at((x_an, y0)).down(2.2).label("1 kΩ", fontsize=FS_SMALL, loc="bottom"))
    d.add(elm.Capacitor().down(1.6).label("10 nF", fontsize=FS_SMALL, loc="bottom"))
    y_capend = y0 - 2.2 - 1.6

    # ---- MAX485 module (receiver only)
    u = d.add(elm.Ic(size=(4.6, 4.0), pins=[
        elm.IcPin(name="A", side="top", slot="1/2", anchorname="A"),
        elm.IcPin(name="B", side="top", slot="2/2", anchorname="B"),
        # left-side slots count from the bottom: VCC ends up on top
        elm.IcPin(name="VCC", side="left", slot="4/4", anchorname="VCC"),
        elm.IcPin(name="DI", side="left", slot="3/4", anchorname="DI"),
        elm.IcPin(name="DE", side="left", slot="2/4", anchorname="DE"),
        elm.IcPin(name="RE", side="left", slot="1/4", anchorname="RE"),
        elm.IcPin(name="RO", side="right", slot="1/1", anchorname="RO"),
        elm.IcPin(name="GND", side="bottom", slot="1/1", anchorname="GND"),
    ], pinspacing=1.0, edgepadW=0.6, edgepadH=0.6, lsize=FS_SMALL, plblsize=FS_SMALL)
        .right().at((x_ap, y_capend - 1.0)).anchor("A")
        .label("MAX485\nmodule\n@ 3.3 V", loc="center", fontsize=FS_SMALL, ofst=(0.3, 0)))
    route(d, (x_ap, y_capend), u.A)
    route(d, (x_an, y_capend), (x_an, y_capend - 0.5), (u.B[0], y_capend - 0.5), u.B)
    # DE + RE tied to GND: receiver always enabled, driver always off
    xl = u.VCC[0] - 1.5
    route(d, u.DE, (xl, u.DE[1]))
    route(d, u.RE, (xl, u.RE[1]))
    d.add(elm.Dot().at((xl, u.DE[1])))
    route(d, (xl, u.DE[1]), (xl, u.RE[1] - 0.4))
    d.add(elm.Ground().at((xl, u.RE[1] - 0.4)))
    route(d, u.DI, (u.DI[0] - 0.5, u.DI[1]))
    text(d, (u.DI[0] - 0.6, u.DI[1]), "NC", mut, FS_SMALL, halign="right")
    xv = u.VCC[0] - 0.8
    route(d, u.VCC, (xv, u.VCC[1]), (xv, u.VCC[1] + 0.5))
    rail(d, (xv, u.VCC[1] + 0.5), t)
    d.add(elm.Ground().at(u.GND))
    text(d, (u.center[0] - 0.3, u.GND[1] - 1.1), "R7 (120 Ω), R5, R6 (bias) removed", acc, FS_SMALL,
         halign="center", valign="top")

    # ---- ESP32-C3 SuperMini
    e = d.add(elm.Ic(size=(6.6, 4.4), pins=[
        elm.IcPin(name=f"GPIO{PIN_AUX_IN}", side="left", slot="1/1", anchorname="AUX"),
        elm.IcPin(name="3V3", side="top", slot="1/3", anchorname="V33"),
        elm.IcPin(name=f"GPIO{PIN_PANEL_VCC}", side="top", slot="2/3", anchorname="VCC"),
        elm.IcPin(name=f"GPIO{PIN_BL}", side="top", slot="3/3", anchorname="BL"),
        elm.IcPin(name="GND", side="bottom", slot="1/1", anchorname="GND"),
        elm.IcPin(name="5V", side="right", slot="1/1", anchorname="V5"),
    ], pinspacing=1.0, edgepadW=1.0, edgepadH=0.6, lsize=FS_SMALL, plblsize=FS_SMALL)
        .right().at((x_gate, -19.0)).anchor("VCC")
        .label(f"ESP32-C3 SuperMini\n\nGPIO{PIN_LED}: onboard LED\n(active low)", loc="center",
               fontsize=FS_SMALL))
    # power: 3V3 pin straight from the adapter rail (regulator bypassed)
    route(d, e.V33, (e.V33[0], e.V33[1] + 0.5))
    rail(d, (e.V33[0], e.V33[1] + 0.5), t)
    d.add(elm.Ground().at(e.GND))
    route(d, e.V5, (e.V5[0] + 0.5, e.V5[1]))
    text(d, (e.V5[0] + 0.6, e.V5[1]), "NC", mut, FS_SMALL)
    # RO -> GPIO4
    xm = (u.RO[0] + e.AUX[0]) / 2
    route(d, u.RO, (xm, u.RO[1]), (xm, e.AUX[1]), e.AUX)
    # gate -> GPIO10, BL_EN -> GPIO5
    route(d, (x_gate, y_pd), e.VCC)
    y_h = -16.0
    route(d, (x_bl, Y_BL), (x_bl, y_h), (e.BL[0], y_h), e.BL)

    text(d, (x_gate + 0.25, -14.4), "PIN_PANEL_VCC\nLOW = panel on\nHIGH = VCC off", mut, FS_SMALL,
         valign="top")
    text(d, (x_bl + 0.25, -14.4), "PIN_BL, open-drain,\nno internal pulls\nLOW = backlight off\nreleased = on",
         mut, FS_SMALL, valign="top")

    # legend
    ly = e.GND[1] - 2.0
    lg = [
        "Pin numbers: 40-pin connector pin, panel pin in parentheses.",
        f"Firmware pins (config.h, C3 build): GPIO{PIN_AUX_IN} = PIN_AUX_IN, GPIO{PIN_BL} = PIN_BL, "
        f"GPIO{PIN_PANEL_VCC} = PIN_PANEL_VCC, GPIO{PIN_LED} = PIN_LED.",
        "3V3_ADP: adapter 3.3 V rail (adapter side of the cut VCC trace). It feeds the SuperMini 3V3 pin",
        "(regulator bypassed, 5V pin unconnected), the MAX485 and every pull-up.",
        "All grounds are the adapter GND (common). Red X: original trace cut. Dashed: no longer driven.",
    ]
    for i, s in enumerate(lg):
        text(d, (XA - 6.0, ly - 0.6 * i), s, mut, FS_SMALL)
    return d


def bl_mosfet(theme):
    """Backlight MOSFET alternative (BL_MODE_MOSFET, not installed)."""
    t = THEMES[theme]
    fg, mut = t["fg"], t["muted"]
    d = schemdraw.Drawing(show=False)
    d.config(color=fg, bgcolor=t["bg"], fontsize=FS, font="sans-serif", lw=1.4, unit=2.5)
    d.add(elm.Dot(open=True).at((0, 0)))
    text(d, (-0.25, 0), f"GPIO{PIN_BL} (PIN_BL)", fg, FS_SMALL, halign="right")
    d.add(elm.Resistor().at((0, 0)).right(2.4).label("100 Ω", fontsize=FS_SMALL))
    n = (2.4, 0)
    d.add(elm.Dot().at(n))
    d.add(elm.Resistor().at(n).down(2.0).label("100 kΩ", fontsize=FS_SMALL, loc="top"))
    d.add(elm.Ground())
    d.add(elm.Line().at(n).right(1.2))
    q = d.add(elm.NFet(bulk=False).reverse().anchor("gate").label("2N7002", fontsize=FS_SMALL, loc="right"))
    d.add(elm.Ground().at(q.source))
    top = (q.drain[0], q.drain[1] + 1.0)
    route(d, q.drain, top, (top[0] + 3.0, top[1]))
    d.add(elm.Dot(open=True).at((top[0] + 3.0, top[1])))
    text(d, (top[0] + 3.25, top[1]), "BL_EN, conn. pin 32 (panel 22)\n(with its pull-up)",
         fg, FS_SMALL)
    text(d, (-3.8, -3.4), "HIGH: MOSFET on, BL_EN pulled low: backlight OFF\n"
         "LOW or unconfigured: 100 kΩ holds the gate low: backlight ON\n"
         "Not installed. Build with EXTRA=\"-DBL_MODE=BL_MODE_MOSFET\" ./build.sh",
         mut, FS_SMALL, valign="top")
    return d


def save(d, path, theme):
    """Save as SVG with an explicit background rect (some viewers ignore the
    root background-color style) and a portable font stack."""
    svg = d.get_imagedata("svg").decode()
    m = re.search(r'viewBox="([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+)"', svg)
    x, y, w, h = m.groups()
    bg = f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{THEMES[theme]['bg']}"/>'
    svg = re.sub(r"(<svg[^>]*>)", lambda mm: mm.group(1) + bg, svg, count=1)
    svg = svg.replace('font-family="sans"', 'font-family="Helvetica, Arial, sans-serif"')
    svg = svg.replace("stroke-dasharray:-;", "")
    path.write_text(svg)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, fn in (("schematic", installed), ("backlight-mosfet", bl_mosfet)):
        for theme in THEMES:
            path = OUT / f"{name}-{theme}.svg"
            save(fn(theme), path, theme)
            print(path.relative_to(OUT.parents[1]))


if __name__ == "__main__":
    schemdraw.use("svg")
    main()

# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
"""Builds the manual's annotated screenshots from real application captures.

    python docs/figures/compose_ui.py [--captures docs/images/captures] [--out docs/images]

The captures are untouched screenshots of the running Standalone (see README.md
for how they were taken). Nothing is drawn into them except numbered callouts
and thin outlines, which are vector shapes laid over the picture; the keys that
explain the numbers sit below the screenshot, never on it.

Writes (SVG + 300 dpi PNG):
    fig01-interface            the whole window, eight numbered regions
    fig06-sample-granular-ui   the Sample and Granular control strips
    fig08-sequencer            the step grid with a gesture key
"""
import argparse
import pathlib
import textwrap

import matplotlib.patheffects as pe
import numpy as np
from PIL import Image

import figstyle as fs
plt = fs.plt


def put_image(fig, img, top_mm, width_mm=160.0, margin_px=0, bottom_px=0):
    """Places img at 160 mm wide (with optional white margin above/below); returns its axes."""
    h_mm = width_mm * (img.shape[0] + margin_px + bottom_px) / img.shape[1]
    H = fig.get_figheight() / fs.MM
    ax = fig.add_axes([0, 1 - (top_mm + h_mm) / H, width_mm / fs.WIDTH_MM, h_mm / H])
    ax.imshow(img, interpolation="none", aspect="auto")
    ax.set_xlim(0, img.shape[1])
    ax.set_ylim(img.shape[0] + bottom_px, -margin_px)
    ax.axis("off")
    return ax, h_mm


def outline(ax, box, color="white"):
    x0, y0, x1, y1 = box
    ax.plot([x0, x1, x1, x0, x0], [y0, y0, y1, y1, y0], color="black", lw=2.0, solid_joinstyle="miter")
    ax.plot([x0, x1, x1, x0, x0], [y0, y0, y1, y1, y0], color=color, lw=0.9, ls=(0, (4, 2)),
            solid_joinstyle="miter")


def marker(ax, x, y, label):
    ax.scatter([x], [y], s=210, color="white", edgecolor="black",
               linewidth=1.2, zorder=5)
    ax.text(x, y, str(label), ha="center", va="center", fontsize=9, fontweight="bold", color="black", zorder=6)


def key_block(fig, items, top_mm, cols=2, wrap=40, line_mm=3.9, gap_mm=1.6):
    """Draws a numbered key below the picture, filled column by column."""
    H = fig.get_figheight() / fs.MM
    per = -(-len(items) // cols)
    col_w = 160 / cols
    for c in range(cols):
        y = top_mm
        for n, text in items[c * per:(c + 1) * per]:
            lines = textwrap.wrap(text, wrap) or [""]
            fig.text((c * col_w + 1.0) / 160, 1 - y / H, str(n), ha="left", va="top", fontsize=9,
                     fontweight="bold")
            fig.text((c * col_w + 6.0) / 160, 1 - y / H, "\n".join(lines), ha="left", va="top", fontsize=9,
                     linespacing=1.2)
            y += len(lines) * line_mm + gap_mm
    return y


def key_height(items, cols=2, wrap=40, line_mm=3.9, gap_mm=1.6):
    per = -(-len(items) // cols)
    return max(sum(len(textwrap.wrap(t, wrap)) * line_mm + gap_mm for _, t in items[c * per:(c + 1) * per])
               for c in range(cols))


# ---------------------------------------------------------------- figure 1
FIG1_REGIONS = [
    (1, (60, 143, 740, 201), (32, 165)),
    (2, (60, 260, 740, 548), (32, 282)),
    (3, (60, 568, 735, 642), (32, 590)),
    (4, (60, 715, 735, 758), (32, 737)),
    (5, (800, 95, 1480, 657), (772, 117)),
    (6, (800, 683, 1480, 764), (772, 705)),
    (7, (60, 800, 1480, 985), (32, 822)),
    (8, (60, 1008, 1480, 1230), (32, 1030)),
]
FIG1_KEY = [
    (1, "Source and waveform. The left menu picks Oscillator, Sample, Granular or Random; the right menu picks the oscillator waveform."),
    (2, "Envelope. ATK, DEC, SUS and REL shape how each note's loudness rises, settles, holds and dies away."),
    (3, "Low-pass filter. The slider sets the cutoff; ON switches the filter in or out."),
    (4, "Voice lights. One circle for each of the 16 voices, lit while that voice is sounding."),
    (5, "Output display. SCOPE or SPECTRUM view of the mix, with left and right level meters."),
    (6, "GAIN sets the output level; WIDTH sets the stereo spread."),
    (7, "SHAPE strip (Oscillator and Random). With Sample or Granular selected it becomes the SAMPLE strip: see Figure 6."),
    (8, "Step sequencer: RUN, SYNC, TEMPO, rate and keyboard mode above; 16 steps below, then SWING and GATE."),
]


def fig01(cap, out):
    img = np.asarray(Image.open(cap / "ui-oscillator-running.png").convert("RGB"))
    key_mm = key_height(FIG1_KEY)
    h_img = 160 * img.shape[0] / img.shape[1]
    fig = fs.figure(h_img + key_mm + 7)
    ax, h_mm = put_image(fig, img, 0)
    for n, box, (mx, my) in FIG1_REGIONS:
        outline(ax, box)
    for n, box, (mx, my) in FIG1_REGIONS:
        marker(ax, mx, my, n)
    key_block(fig, FIG1_KEY, h_mm + 3.5)
    return fs.save(fig, out, "fig01-interface")


# ------------------------------------------------------------- figure 6a
SAMPLE_REGIONS = [
    (1, (600, 800, 1160, 845), (600, 800)),      # LOAD, PAD, loop menu
    (2, (60, 857, 840, 980), (60, 857)),         # waveform view
    (3, (868, 858, 968, 985), (918, 1024)),      # ROOT
    (4, (992, 858, 1092, 985), (1042, 1024)),    # START
    (5, (1116, 858, 1216, 985), (1166, 1024)),   # L.START
    (6, (1240, 858, 1340, 985), (1290, 1024)),   # L.END
    (7, (1364, 858, 1464, 985), (1414, 1024)),   # XFADE
]
SAMPLE_KEY = [
    (1, "LOAD and PAD fill the sample; the menu beside them chooses the loop mode (One-shot, Loop, Loop until release)."),
    (2, "The sample as a waveform. The brackets mark L.START and L.END."),
    (3, "ROOT: the key that plays the sample at its original speed."),
    (4, "START: where each note begins reading."),
    (5, "L.START: where the loop begins."),
    (6, "L.END: where the loop ends."),
    (7, "XFADE: the crossfade at the loop end."),
]
GRAN_REGIONS = [
    (1, (600, 800, 1160, 845), (600, 800)),
    (2, (60, 857, 840, 980), (60, 857)),
    (3, (858, 858, 942, 985), (900, 1024)),
    (4, (946, 858, 1030, 985), (988, 1024)),
    (5, (1034, 858, 1118, 985), (1076, 1024)),
    (6, (1122, 858, 1206, 985), (1164, 1024)),
    (7, (1210, 858, 1294, 985), (1252, 1024)),
    (8, (1298, 858, 1382, 985), (1340, 1024)),
    (9, (1386, 858, 1470, 985), (1428, 1024)),
]
GRAN_KEY = [
    (1, "LOAD and PAD fill the sample; the loop menu is dimmed in Granular."),
    (2, "The sample as a waveform. The magenta line is POS; the shaded band is the reach of SPRAY."),
    (3, "ROOT: the sample's pitch reference."),
    (4, "POS: where grains are taken from."),
    (5, "SCAN: how fast POS moves through the file. Negative values move it backwards; frozen = 0."),
    (6, "SIZE: grain length."),
    (7, "DENS: grains started each second."),
    (8, "SPRAY: how widely grains scatter around POS."),
    (9, "JITTER: random pitch variation per grain."),
]
STRIP = (0, 770, 1520, 1000)
STRIP_BELOW = 50                     # white strip under the capture for the knob callouts


def strip(fig, img, top_mm, regions):
    x0, y0, x1, y1 = STRIP
    crop = img[y0:y1, x0:x1]
    ax, h = put_image(fig, crop, top_mm, bottom_px=STRIP_BELOW)
    shifted = [(n, (a - x0, b - y0, c - x0, d - y0), (mx - x0, my - y0)) for n, (a, b, c, d), (mx, my) in regions]
    for n, box, _ in shifted:
        outline(ax, box)
    for n, box, (mx, my) in shifted:
        marker(ax, mx, my, n)
    return h


def fig06a(cap, out):
    sample = np.asarray(Image.open(cap / "ui-sample.png").convert("RGB"))
    gran = np.asarray(Image.open(cap / "ui-granular.png").convert("RGB"))
    strip_mm = 160 * (STRIP[3] - STRIP[1] + STRIP_BELOW) / 1520
    kh1, kh2 = key_height(SAMPLE_KEY), key_height(GRAN_KEY)
    title_mm = 7
    total = title_mm + strip_mm + 3 + kh1 + 6 + title_mm + strip_mm + 3 + kh2 + 2
    fig = fs.figure(total)
    H = total
    y = 0
    fig.text(0.0, 1 - 4.5 / H, "a  Source: Sample", fontsize=10, fontweight="bold", va="center")
    y += title_mm
    h = strip(fig, sample, y, SAMPLE_REGIONS)
    key_block(fig, SAMPLE_KEY, y + h + 3)
    y += h + 3 + kh1 + 6
    fig.text(0.0, 1 - (y + 4.5) / H, "b  Source: Granular", fontsize=10, fontweight="bold", va="center")
    y += title_mm
    h = strip(fig, gran, y, GRAN_REGIONS)
    key_block(fig, GRAN_KEY, y + h + 3)
    return fs.save(fig, out, "fig06-sample-granular-ui")


# ---------------------------------------------------------------- figure 7
SEQ_CROP = (40, 1000, 1500, 1240)
SEQ_MARGIN = 95                      # white strip above the capture, for the callouts
SEQ_MARKS = [                        # number, marker x, target (x, y) in full-capture pixels
    (1, 105, (105, 1100)),           # enabled step
    (2, 193, (193, 1100)),           # disabled step
    (3, 282, (282, 1119)),           # note name
    (4, 370, (370, 1167)),           # velocity bar
    (5, 726, (726, 1100)),           # playhead (cyan outline)
]
SEQ_KEY = [
    (1, "Enabled step: bright amber. Every fourth step has a brighter top edge, which marks the beat."),
    (2, "Disabled step: dark, with a dim note name. Its note and velocity are kept."),
    (3, "Note name: the pitch the step plays."),
    (4, "Velocity bar: its length is the step's velocity."),
    (5, "Playhead: a cyan outline on the step now playing."),
]
SEQ_GESTURES = [
    ("Click", "turn the step on or off"),
    ("Drag up or down", "change the note: one semitone for every 6 pixels"),
    ("Shift-drag or right-drag", "change the velocity, up or down"),
    ("Mouse wheel", "note up or down by one semitone"),
    ("Shift + mouse wheel", "velocity up or down by 4"),
]


def fig07(cap, out):
    full = np.asarray(Image.open(cap / "ui-oscillator-running.png").convert("RGB"))
    x0, y0, x1, y1 = SEQ_CROP
    crop = full[y0:y1, x0:x1]
    h_img = 160 * (crop.shape[0] + SEQ_MARGIN) / crop.shape[1]
    key_mm = key_height(SEQ_KEY, cols=1, wrap=80)
    H = h_img + 3 + key_mm + 4 + len(SEQ_GESTURES) * 4.4 + 8
    fig = fs.figure(H)
    ax, h = put_image(fig, crop, 0, margin_px=SEQ_MARGIN)
    for n, mx, (tx, ty) in SEQ_MARKS:
        X, Y = mx - x0, -SEQ_MARGIN * 0.5
        tX, tY = tx - x0, ty - y0
        ax.plot([X, tX], [Y, tY], color="white", lw=3.0, zorder=2)
        ax.plot([X, tX], [Y, tY], color="black", lw=1.0, zorder=3)
        ax.scatter([tX], [tY], s=18, color="black", edgecolor="white", linewidth=1.0, zorder=4)
        marker(ax, X, Y, n)
    key_block(fig, SEQ_KEY, h + 3, cols=1, wrap=80)
    y = h + 3 + key_mm + 4
    fig.text(0.0, 1 - y / H, "Mouse and wheel gestures on a step", fontsize=10, fontweight="bold", va="top")
    y += 6
    for g, t in SEQ_GESTURES:
        fig.text(1.0 / 160, 1 - y / H, g, fontsize=9, fontweight="bold", va="top")
        fig.text(52.0 / 160, 1 - y / H, t, fontsize=9, va="top")
        y += 4.4
    return fs.save(fig, out, "fig08-sequencer")


def main():
    here = pathlib.Path(__file__).resolve().parent
    ap = argparse.ArgumentParser()
    ap.add_argument("--captures", type=pathlib.Path, default=here.parent / "images" / "captures")
    ap.add_argument("--out", type=pathlib.Path, default=here.parent / "images")
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    for fn in (fig01, fig06a, fig07):
        print(fn.__name__, [p.name for p in fn(a.captures, a.out)])


if __name__ == "__main__":
    main()

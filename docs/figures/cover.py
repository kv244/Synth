# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
"""Draws the manual's cover: docs/images/cover.svg and cover.png (A4 at 300 dpi).

    python docs/figures/cover.py [--out docs/images]

The picture is the instrument's idea in one image: a stack of single-cycle
waves that morphs from sine through triangle and square to sawtooth, over a
receding grid, with the 16-step row underneath. Everything is drawn by this
script (no stock art); the glow is layered strokes, so the SVG has no filters.
"""
import argparse
import pathlib

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from matplotlib.colors import LinearSegmentedColormap, to_rgb  # noqa: E402
from matplotlib.patches import FancyBboxPatch  # noqa: E402

W_MM, H_MM = 210.0, 297.0
CYAN, MAGENTA, AMBER = "#19e6ff", "#ff2fd0", "#ffb319"
BG_TOP, BG_BOTTOM = "#03050b", "#0b0620"

plt.rcParams.update({"font.family": "DejaVu Sans", "svg.fonttype": "path", "svg.hashsalt": "neonsynth-cover"})


def glow(ax, x, y, color, lw=1.1, alpha=1.0, zorder=3):
    for width, a in ((9.0, 0.035), (5.5, 0.07), (3.0, 0.16), (1.0, 1.0)):
        ax.plot(x, y, color=color, lw=lw * width, alpha=a * alpha, solid_capstyle="round",
                solid_joinstyle="round", zorder=zorder)


def wave(kind, ph):
    """One of the four shapes at phase ph (cycles), range -1..1."""
    t = ph % 1.0
    if kind == 0:
        return np.sin(2 * np.pi * t)
    if kind == 1:
        return 1 - 4 * np.abs(((t + 0.25) % 1.0) - 0.5)
    if kind == 2:
        return np.where(t < 0.5, 1.0, -1.0)
    return 2 * ((t + 0.5) % 1.0) - 1


def mix(c0, c1, u):
    a, b = np.array(to_rgb(c0)), np.array(to_rgb(c1))
    return tuple(a + (b - a) * u)


def spaced(text):
    return " ".join(text)          # thin spaces: wide, technical letter-spacing


def draw(out_dir):
    fig = plt.figure(figsize=(W_MM / 25.4, H_MM / 25.4))
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set_xlim(0, W_MM)
    ax.set_ylim(0, H_MM)
    ax.axis("off")

    # background: vertical gradient with a soft horizon glow
    horizon = 92.0
    yy = np.linspace(0, 1, 600)[:, None]
    grad = LinearSegmentedColormap.from_list("bg", [BG_BOTTOM, BG_TOP])(yy)[:, :, :3]
    bump = np.exp(-((yy * H_MM - horizon) / 16.0) ** 2) * 0.16
    grad = np.clip(grad + bump[:, :, None] * np.array(to_rgb(MAGENTA)), 0, 1)
    ax.imshow(np.repeat(grad, 2, axis=1), extent=(0, W_MM, 0, H_MM), origin="lower", aspect="auto",
              interpolation="bicubic", zorder=0)

    # receding grid below the horizon
    cx = W_MM / 2
    for k in range(-14, 15):
        xb = cx + k * 26.0
        ax.plot([cx + k * 1.2, xb], [horizon, 0], color=MAGENTA, lw=0.5, alpha=0.30, zorder=1)
    for i in range(1, 15):
        y = horizon - horizon * (i / 14.0) ** 2.2
        ax.plot([0, W_MM], [y, y], color=MAGENTA, lw=0.5, alpha=0.10 + 0.25 * (i / 14.0), zorder=1)
    glow(ax, [0, W_MM], [horizon, horizon], MAGENTA, lw=0.7, alpha=0.9, zorder=2)

    # the stack: sine -> triangle -> square -> sawtooth
    rows, top, bottom = 28, 222.0, 104.0
    x = np.linspace(14, W_MM - 14, 1600)
    s = (x - x[0]) / (x[-1] - x[0])
    window = np.sin(np.pi * s) ** 1.6                    # lines lie flat at the edges
    ph = (s - 0.5) * 3.0                                 # three cycles across the page
    for r in range(rows):
        u = r / (rows - 1) * 3.0                         # 0..3 through the four shapes
        k = min(int(u), 2)
        f = u - k
        f = f * f * (3 - 2 * f)                          # ease between shapes
        y = (1 - f) * wave(k, ph) + f * wave(k + 1, ph)
        base = top + (bottom - top) * r / (rows - 1)
        color = mix(CYAN, MAGENTA, u / 1.5) if u < 1.5 else mix(MAGENTA, AMBER, (u - 1.5) / 1.5)
        strong = (r % 9 == 0)                            # the four pure shapes stand out
        glow(ax, x, base + y * window * 6.4, color, lw=1.25 if strong else 0.62,
             alpha=1.0 if strong else 0.72, zorder=3 + r)

    for r, name in ((0, "SINE"), (9, "TRIANGLE"), (18, "SQUARE"), (27, "SAWTOOTH")):
        base = top + (bottom - top) * r / (rows - 1)
        ax.text(W_MM - 12, base + 1.6, spaced(name), color="#cfd8e6", fontsize=6.5, ha="right", va="bottom",
                alpha=0.85, zorder=40)

    # title block
    ax.text(cx, 263, spaced("NEON SYNTH"), color="white", fontsize=44, fontweight="bold", ha="center",
            va="center", zorder=50)
    glow(ax, [cx - 62, cx + 62], [249.5, 249.5], CYAN, lw=0.8, zorder=50)
    ax.text(cx, 241.5, spaced("USER MANUAL"), color=CYAN, fontsize=15, ha="center", va="center", zorder=50)
    ax.text(cx, 231.5, "A learning synthesizer: hear a waveform, see it, shape it",
            color="#cfd8e6", fontsize=9.5, ha="center", va="center", style="italic", zorder=50)

    # the 16-step row (the instrument's default pattern: every other step on)
    n, size, gap = 16, 8.2, 2.6
    x0 = cx - (n * size + (n - 1) * gap) / 2
    for i in range(n):
        on = i % 2 == 0
        ax.add_patch(FancyBboxPatch((x0 + i * (size + gap), 31), size, size,
                                    boxstyle="round,pad=0,rounding_size=1.2", linewidth=0.8,
                                    edgecolor=AMBER if on else "#5a6478",
                                    facecolor=AMBER if on else "none", alpha=0.95 if on else 0.8, zorder=45))
    ax.text(cx, 19, spaced("RAZVAN JULIAN PETRESCU") + "   /   " + spaced("AUDIODSP"), color="#cfd8e6",
            fontsize=8, ha="center", va="center", zorder=50)

    out_dir.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_dir / "cover.svg", metadata={"Date": None, "Creator": None})
    fig.savefig(out_dir / "cover.png", dpi=300, metadata={"Software": None})
    plt.close(fig)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parent.parent / "images")
    draw(ap.parse_args().out)
    print("wrote cover.svg and cover.png")

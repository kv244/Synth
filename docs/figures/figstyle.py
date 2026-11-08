# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
"""Shared look for the manual's figures.

Every figure is drawn 160 mm wide (the print text width) with 9 pt labels and
10 pt panel titles, in DejaVu Sans (bundled with matplotlib, so a rebuild looks
the same everywhere). Text is stored as outlines in the SVGs. Lines are told
apart by style and labels as well as colour, so the figures read in grayscale.
"""
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

MM = 1 / 25.4                     # inches per millimetre
WIDTH_MM = 160
PNG_DPI = 300                     # 160 mm at 300 dpi = 1890 px

INK = "#111111"                   # main data line
GREY = "#7a7a7a"                  # reference / "before" line
LIGHT = "#d9d9d9"                 # bands and fills
ACCENT = "#0a6e8a"                # dark cyan: reads as a mid grey in grayscale
AMBER = "#b36b00"                 # dark amber: reads darker than ACCENT

plt.rcParams.update({
    "font.family": "DejaVu Sans",
    "font.size": 9,
    "axes.titlesize": 10,
    "axes.titleweight": "bold",
    "axes.labelsize": 9,
    "xtick.labelsize": 8.5,
    "ytick.labelsize": 8.5,
    "legend.fontsize": 8.5,
    "axes.linewidth": 0.6,
    "xtick.major.width": 0.6,
    "ytick.major.width": 0.6,
    "lines.linewidth": 1.0,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "svg.fonttype": "path",
    "svg.hashsalt": "neonsynth",  # stable ids, so a rebuild gives the same file
    "savefig.dpi": PNG_DPI,
    "figure.dpi": 100,
})


def figure(height_mm, **kwargs):
    return plt.figure(figsize=(WIDTH_MM * MM, height_mm * MM), **kwargs)


def save(fig, out_dir, name):
    """Writes name.svg and name.png (300 dpi); returns the two paths."""
    svg, png = out_dir / f"{name}.svg", out_dir / f"{name}.png"
    fig.savefig(svg, metadata={"Date": None, "Creator": None})
    fig.savefig(png, metadata={"Software": None})
    plt.close(fig)
    return svg, png

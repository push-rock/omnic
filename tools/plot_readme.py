#!/usr/bin/env python3
"""Charts for the tables in README.md / BENCHMARK.md.

Every number below is a table value from those two pages (same windows, zero
drift).  The only derived text is the "Nx faster / Nx slower" label, which is
that row's time against zip's, and the heatmap shade, which is a cell's ratio
against the best pick in its row.

    python tools/plot_readme.py            # writes into assets/

        archive_silesia.png    Silesia table    -> size, pack, unpack
        archive_gametree.png   game-tree table  -> size, pack, unpack
        per_type_picks.png     per-file table   -> heatmap

Design notes: each figure is ~9.9 in wide at 200 dpi, which GitHub shows at
~880 px, so an 11 pt label lands at ~13 px on the page.  Rows are options,
panels are metrics, the dashed line is zip: lower is better to the left.
The script ends with a layout check (text outside the figure, overlapping
text) and exits non-zero if it finds any.
"""
import math
import os
import sys

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
import matplotlib.text
import matplotlib.transforms
import numpy as np
from matplotlib.colors import LinearSegmentedColormap, PowerNorm
from matplotlib.ticker import FixedLocator, FuncFormatter, NullLocator

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.normpath(os.path.join(HERE, "..", "assets"))

OMNIC = "#D55E00"   # omnic: the project's accent (Okabe-Ito vermillion)
ZIP = "#4d4d4d"     # zip: the reference in every panel
SEVENZ = "#9e9e9e"  # 7-Zip
TXT = "#262626"
MUTED = "#6b6b6b"
GRID = "#e6e6e6"

plt.rcParams.update({
    "font.family": "sans-serif",
    "font.sans-serif": ["Segoe UI", "Arial", "Helvetica Neue", "DejaVu Sans"],
    "font.size": 11,
    "text.color": TXT,
    "figure.dpi": 200,
    "savefig.dpi": 200,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.spines.left": False,
})

TOTAL_W = 9.85  # inches; GitHub shows this at ~880 px

# ----------------------------------------------------------------- tables --
# label, who, size, vs zip, pack, [unpack]   (strings are the table's own text)

SILESIA = {
    "file": "archive_silesia.png",
    "title": "Silesia corpus: 202 MiB, 12 files",
    "sub": "Every panel: lower is better (to the left). The dashed line marks zip.",
    "foot": ("i7-12700, Windows. Pack: best of 3, zip / 7-Zip / omnic interleaved in one window.\n"
             "Unpack: best of 5, measured later the same way. Thread counts not equalised. "
             "\u00d7 faster / slower is against zip."),
    "rows": [
        ("zip (deflate)", "zip", "65.0 MB", None, "4.99 s", "0.38 s"),
        ("7z -mx=5 (default)", "7z", "47.3 MB", "-27.3%", "21.6 s", "0.93 s"),
        ("7z -mx=9 (max)", "7z", "46.4 MB", "-28.6%", "46.3 s", "1.43 s"),
        ("omnic fast_compress", "omnic", "82.9 MB", "+27.5%", "0.34 s", "0.20 s"),
        ("omnic fast_decompress", "omnic", "63.4 MB", "-2.5%", "0.44 s", "0.20 s"),
        ("omnic pack (default)", "omnic", "58.6 MB", "-10.0%", "0.76 s", "0.20 s"),
        ("omnic -p best_ratio", "omnic", "46.5 MB", "-28.5%", "30.2 s", "0.62 s"),
    ],
}

GAMETREE = {
    "file": "archive_gametree.png",
    "lab_w": 1.80,
    "title": "Game tree: 152.6 MB, 8,586 files",
    "sub": ("PNG textures, Lua scripts, Ogg audio, fonts, DLLs. "
            "Every panel: lower is better (to the left). The dashed line marks zip."),
    "foot": ("i7-12700, Windows. Pack: best of 3, zip / 7-Zip / omnic interleaved in one window.\n"
             "Unpack: best of 5, measured later the same way; this figure swings by a third between sessions.\n"
             "Thread counts not equalised. \u00d7 faster / slower is against zip."),
    "rows": [
        ("zip (deflate)", "zip", "100.7 MB", None, "3.86 s", "2.64 s"),
        ("7z -mx=5", "7z", "85.5 MB", "-15.1%", "13.4 s", "6.41 s"),
        ("7z -mx=9", "7z", "84.7 MB", "-15.8%", "14.9 s", "6.50 s"),
        ("omnic fast_compress", "omnic", "101.5 MB", "+0.7%", "1.40 s", "3.72 s"),
        ("omnic fast_decompress", "omnic", "96.9 MB", "-3.8%", "1.52 s", "4.21 s"),
        ("omnic pack (default)", "omnic", "95.7 MB", "-5.0%", "1.55 s", "4.08 s"),
        ("omnic --per-file", "omnic", "99.6 MB", "-1.1%", "1.47 s", "4.03 s"),
        ("omnic -p best_ratio", "omnic", "92.0 MB", "-8.6%", "11.3 s", "4.49 s"),
        ("omnic --no-skip", "omnic", "90.5 MB", "-10.2%", "12.5 s", "4.71 s"),
    ],
}

# file, kind, omnic balanced, omnic best_ratio, zip deflate, zip -9 (7z), 7z -mx=9
PICKS_COLS = ["omnic\nbalanced", "omnic\nbest_ratio", "zip\ndeflate", "zip -9\n(7z)", "7z\n-mx=9"]
PICKS_ROWS = [
    ("dickens", "novel text", 33.54, 27.96, 37.99, 36.12, 27.78),
    ("mozilla", "executables", 33.79, 29.45, 37.25, 35.58, 26.05),
    ("mr", "recorded data", 33.95, 31.16, 36.87, 34.63, 27.56),
    ("nci", "chemical DB", 7.23, 4.96, 9.54, 8.23, 5.19),
    ("ooffice", "office binary", 47.79, 42.21, 50.33, 48.68, 39.43),
    ("osdb", "SQL DB", 33.67, 30.74, 36.62, 35.87, 28.28),
    ("reymont", "novel text", 26.74, 20.33, 28.07, 25.62, 19.89),
    ("samba", "source code", 21.20, 18.03, 25.22, 23.70, 17.40),
    ("sao", "star coords", 72.70, 68.95, 73.53, 72.24, 60.87),
    ("webster", "dictionary", 26.07, 20.94, 29.45, 27.87, 20.23),
    ("x-ray", "medical img", 65.51, 60.53, 71.34, 67.81, 52.86),
    ("xml", "XML", 10.23, 8.47, 12.87, 11.85, 8.51),
]


def rel(value, ref):
    """A time against the reference time: which is faster, and by how much."""
    r = ref / value
    if r >= 1.1:
        return f"{r:.1f}\u00d7 faster"
    if r <= 1 / 1.1:
        return f"{1 / r:.1f}\u00d7 slower"
    return "~ zip"


def num(s):
    return float(s.split()[0])


# ------------------------------------------------------------- QA helper ---
def qa(fig, name):
    """Text outside the figure, or two texts overlapping: report, don't guess."""
    fig.canvas.draw()
    r = fig.canvas.get_renderer()
    W, H = fig.canvas.get_width_height()
    boxes = []
    for t in fig.findobj(matplotlib.text.Text):
        if not t.get_visible() or not t.get_text().strip():
            continue
        boxes.append((t.get_text().replace("\n", " / "), t.get_window_extent(r)))
    issues = []
    for s, bb in boxes:
        if bb.x0 < -1 or bb.y0 < -1 or bb.x1 > W + 1 or bb.y1 > H + 1:
            issues.append(f"OUT OF FIGURE  {s!r}  x[{bb.x0:.0f},{bb.x1:.0f}] y[{bb.y0:.0f},{bb.y1:.0f}] of {W}x{H}")
    for i in range(len(boxes)):
        for j in range(i + 1, len(boxes)):
            a, b = boxes[i][1], boxes[j][1]
            ix = min(a.x1, b.x1) - max(a.x0, b.x0)
            iy = min(a.y1, b.y1) - max(a.y0, b.y0)
            if ix > 2 and iy > 2:
                issues.append(f"OVERLAP  {boxes[i][0]!r} x {boxes[j][0]!r}  ({ix:.0f} x {iy:.0f} px)")
    print(f"  QA {name}: {W}x{H} px, {len(boxes)} text items, {len(issues)} issue(s)")
    for s in issues:
        print("    " + s)
    return issues


# -------------------------------------------------------------- tier chart --
def tier_chart(spec, panels):
    rows = spec["rows"]
    zip_row = next(r for r in rows if r[1] == "zip")

    ys, y = [], 0.0
    for i, r in enumerate(rows):
        if i and r[1] == "omnic" and rows[i - 1][1] != "omnic":
            y -= 0.5  # a gap between the competitors and omnic
        y -= 1.0
        ys.append(y)
    ymin, ymax = ys[-1] - 0.6, ys[0] + 0.6

    ROW_IN, TOP_IN, BOT_IN, GAP = 0.36, 1.2, 0.92, 0.20
    LAB_W = spec.get("lab_w", 1.75)
    fig_h = (ymax - ymin) * ROW_IN + TOP_IN + BOT_IN

    cols, widths = ["lab"], [LAB_W]
    for k, p in enumerate(panels):
        if k:
            cols.append("gap")
            widths.append(GAP)
        cols += ["plot", "val"]
        widths += [p["w"], p["vw"]]

    fig = plt.figure(figsize=(TOTAL_W, fig_h))
    gs = fig.add_gridspec(1, len(cols), width_ratios=widths, left=0.012, right=0.995,
                          top=1 - TOP_IN / fig_h, bottom=BOT_IN / fig_h, wspace=0.0)

    def ax_at(kind):
        """The next axes column of this kind, in order (a kind can occur more than once)."""
        n = ax_at.seen.get(kind, 0)
        ax_at.seen[kind] = n + 1
        return fig.add_subplot(gs[0, [i for i, c in enumerate(cols) if c == kind][n]])
    ax_at.seen = {}

    # row labels
    axl = ax_at("lab")
    axl.set_ylim(ymin, ymax)
    axl.axis("off")
    for r, yy in zip(rows, ys):
        om = r[1] == "omnic"
        axl.text(0.99, yy, r[0], transform=axl.get_yaxis_transform(), ha="right", va="center",
                 fontsize=11, color=OMNIC if om else TXT, fontweight="bold" if om else "normal")

    COLOR = {"zip": ZIP, "7z": SEVENZ, "omnic": OMNIC}
    plots, vals = [], []
    for p in panels:
        axp, axv = ax_at("plot"), ax_at("val")
        plots.append(axp)
        vals.append(axv)
        axv.set_ylim(ymin, ymax)
        axv.axis("off")
        if p.get("log"):
            axp.set_xscale("log")
        axp.set_xlim(*p["lim"])
        axp.set_ylim(ymin, ymax)
        axp.xaxis.set_major_locator(FixedLocator(p["ticks"]))
        axp.xaxis.set_minor_locator(NullLocator())
        axp.xaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}"))
        axp.tick_params(axis="x", length=3, pad=3, labelsize=10)
        axp.tick_params(axis="y", left=False, labelleft=False)
        axp.spines["bottom"].set_color("#bcbcbc")
        axp.grid(axis="x", color=GRID, lw=0.9, zorder=0)
        axp.set_axisbelow(True)
        axp.set_title(p["title"], loc="left", pad=20, fontsize=11.5, fontweight="bold")
        axp.set_xlabel(p["xlabel"], fontsize=9.5, color=MUTED, loc="left")

        ref = num(p["key"](zip_row))
        axp.axvline(ref, color=ZIP, lw=1.0, ls=(0, (4, 3)), zorder=1)
        axp.text(ref, 1.0, "zip", transform=axp.get_xaxis_transform(), ha="center", va="bottom",
                 fontsize=9, color=MUTED)

        markers = []
        for r, yy in zip(rows, ys):
            v = num(p["key"](r))
            axp.plot([v], [yy], marker="o", ms=9.5, color=COLOR[r[1]], mec="white", mew=1.3,
                     zorder=3, clip_on=False)
            markers.append((axp, v, yy))
            axv.text(p["va"], yy, p["key"](r), transform=axv.get_yaxis_transform(), ha="right",
                     va="center", fontsize=10.5, color=TXT)
            axv.text(p["ea"], yy, p["extra"](r, zip_row), transform=axv.get_yaxis_transform(),
                     ha="left", va="center", fontsize=10.5, color=MUTED)

    fig.text(0.012, 1 - 0.22 / fig_h, spec["title"], ha="left", va="center",
             fontsize=13.5, fontweight="bold")
    fig.text(0.012, 1 - 0.54 / fig_h, spec["sub"], ha="left", va="center", fontsize=10.5, color=MUTED)
    fig.text(0.012, 0.10 / fig_h, spec["foot"], ha="left", va="bottom", fontsize=8.5, color=MUTED)

    fig.savefig(os.path.join(ASSETS, spec["file"]), facecolor="white")
    issues = qa(fig, spec["file"])

    # a dot must not sit under any text (qa only compares text with text)
    fig.canvas.draw()
    rend = fig.canvas.get_renderer()
    rad = 9.5 / 2 * fig.dpi / 72
    texts = [(t.get_text(), t.get_window_extent(rend)) for t in fig.findobj(matplotlib.text.Text)
             if t.get_visible() and t.get_text().strip()]
    dot_issues = []
    for axp, x, yy in markers:
        px_, py_ = axp.transData.transform((x, yy))
        mbb = matplotlib.transforms.Bbox([[px_ - rad, py_ - rad], [px_ + rad, py_ + rad]])
        for s, tb in texts:
            if matplotlib.transforms.Bbox.overlaps(mbb, tb):
                dot_issues.append(f"DOT UNDER TEXT  at ({px_:.0f},{py_:.0f}) px x {s!r}")
    for s in dot_issues:
        print("    " + s)
    issues += dot_issues
    if "--layout" in sys.argv:
        fig.canvas.draw()
        r = fig.canvas.get_renderer()
        print(f"  layout {spec['file']}  ({TOTAL_W:.2f} x {fig_h:.2f} in)")
        labels = [t.get_window_extent(r) for t in axl.texts]
        print(f"    row labels span x[{min(x.x0 for x in labels) / fig.dpi:.2f},{max(x.x1 for x in labels) / fig.dpi:.2f}] in"
              f"   (figure edge 0.00, label column ends {axl.get_position().x1 * TOTAL_W:.2f})")
        for p, axp in zip(panels, plots):
            x0, w = (axp.get_position().x0 * TOTAL_W, axp.get_position().width * TOTAL_W)
            lo, hi = p["lim"]
            marks = []
            for r_, yy in zip(rows, ys):
                v = num(p["key"](r_))
                f = (math.log10(v / lo) / math.log10(hi / lo)) if p.get("log") else (v - lo) / (hi - lo)
                marks.append(f"{r_[1]}:{f * 100:.0f}%")
            print(f"    {p['title']:<24} plot x[{x0:.2f},{x0 + w:.2f}] in   dots at axis %: " + "  ".join(marks))
        for k, (p, axv) in enumerate(zip(panels, vals)):
            b = axv.get_position()
            col0, col1 = b.x0 * TOTAL_W, b.x1 * TOTAL_W
            xs = [t.get_window_extent(r) for t in axv.texts]
            left, right = min(x.x0 for x in xs) / fig.dpi, max(x.x1 for x in xs) / fig.dpi
            nxt = plots[k + 1].get_position().x0 * TOTAL_W if k + 1 < len(panels) else TOTAL_W
            verdict = "OK" if right <= col1 else ("into the gap only" if right <= nxt else "INTO THE NEXT PANEL")
            print(f"    {p['title']:<24} value column x[{col0:.2f},{col1:.2f}] in   text spans [{left:.2f},{right:.2f}]   {verdict}")
    plt.close(fig)
    return issues


def size_panel(w):
    return {"key": lambda r: r[2], "w": w, "vw": 1.38, "va": 0.47, "ea": 0.54,
            "title": "Archive size", "xlabel": "MB  (smaller is better)",
            "extra": lambda r, z: r[3] or "zip"}


def pack_panel(w):
    return {"key": lambda r: r[4], "w": w, "vw": 1.70, "va": 0.40, "ea": 0.47, "log": True,
            "title": "Pack time  (log scale)", "xlabel": "seconds  (faster is better)",
            "extra": lambda r, z: "zip" if r is z else rel(num(r[4]), num(z[4]))}


def unpack_panel(w):
    return {"key": lambda r: r[5], "w": w, "vw": 1.62, "va": 0.40, "ea": 0.47,
            "title": "Unpack time", "xlabel": "seconds  (faster is better)",
            "extra": lambda r, z: "zip" if r is z else rel(num(r[5]), num(z[5]))}


# ----------------------------------------------------------------- heatmap --
def picks_heatmap():
    heat = LinearSegmentedColormap.from_list(
        "omnic_blue", ["#f7fbff", "#ddeaf6", "#b8d3ea", "#8db9da", "#5a97c8", "#2f6fb1", "#0b3d75"])
    excess = [[(v - min(r[2:])) / min(r[2:]) * 100 for v in r[2:]] for r in PICKS_ROWS]
    VMAX = 45.0  # percent over the best pick; anything larger (one row) saturates
    heat_lum = {}  # norm value -> luminance of the shaded cell, for the ink choice
    for t in np.linspace(0, 1, 101):
        c = heat(t)[:3]
        heat_lum[t] = 255 * (0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2])

    LAB_W, CELL_W, CELL_H, TOP_IN, BOT_IN = 2.0, 1.55, 0.31, 1.25, 0.80
    fig_w = LAB_W + len(PICKS_COLS) * CELL_W + 0.15
    fig_h = TOP_IN + len(PICKS_ROWS) * CELL_H + BOT_IN
    fig = plt.figure(figsize=(fig_w, fig_h))
    ax = fig.add_axes([LAB_W / fig_w, BOT_IN / fig_h, len(PICKS_COLS) * CELL_W / fig_w,
                       len(PICKS_ROWS) * CELL_H / fig_h])
    norm = PowerNorm(gamma=0.8, vmin=0, vmax=VMAX)
    ax.imshow(np.clip(excess, 0, VMAX), cmap=heat, norm=norm, aspect="auto", interpolation="nearest")

    def cell_lum(e):
        t = float(norm(min(e, VMAX)))
        return t, heat_lum[min(heat_lum, key=lambda k: abs(k - t))]

    for i, r in enumerate(PICKS_ROWS):
        ax.text(-0.05, i, f"{r[0]}  \u00b7  {r[1]}", transform=ax.get_yaxis_transform(),
                ha="right", va="center", fontsize=11)
        best = min(r[2:])
        for j, v in enumerate(r[2:]):
            _, lum = cell_lum(excess[i][j])
            # ink by contrast: below ~118 the cell is dark enough for white, above it black reads better
            ax.text(j, i, f"{v:.2f}%", ha="center", va="center", fontsize=11,
                    color="white" if lum < 118 else "#111111", fontweight="bold" if v == best else "normal")
    ax.set_xticks(range(len(PICKS_COLS)))
    ax.set_xticklabels(PICKS_COLS, fontsize=11)
    ax.xaxis.tick_top()  # column headers sit above the grid, under the title
    for k, t in enumerate(ax.get_xticklabels()):
        if k < 2:
            t.set_color(OMNIC)
            t.set_fontweight("bold")
    ax.tick_params(axis="x", length=0, pad=7)
    ax.tick_params(axis="y", length=0, labelleft=False)
    for s in ax.spines.values():
        s.set_visible(False)
    for k in range(1, len(PICKS_ROWS)):
        ax.plot([-0.5, len(PICKS_COLS) - 0.5], [k - 0.5, k - 0.5], color="white", lw=2.2)
    for k in range(1, len(PICKS_COLS)):
        ax.plot([k - 0.5, k - 0.5], [-0.5, len(PICKS_ROWS) - 0.5], color="white", lw=2.2)

    # shade legend: best ... +15% ... +30% ... +45% or more
    ly, sh = 0.30 / fig_h, 0.15 / fig_h
    x = 0.12 / fig_w
    t = fig.text(x, ly + sh / 2, "Shade: how much larger than the best pick for that file",
                 ha="left", va="center", fontsize=9.5, color=MUTED)
    fig.canvas.draw()
    x += t.get_window_extent(fig.canvas.get_renderer()).width / fig_w / fig.dpi + 0.18 / fig_w
    for lab, val in [("best", 0), ("+15%", 15), ("+30%", 30), ("+45% or more", 45)]:
        fig.add_artist(plt.Rectangle((x, ly), 0.28 / fig_w, sh, transform=fig.transFigure,
                                     facecolor=heat(norm(val)), edgecolor="#c8c8c8", lw=0.6))
        t = fig.text(x + 0.36 / fig_w, ly + sh / 2, lab, ha="left", va="center", fontsize=9.5, color=MUTED)
        fig.canvas.draw()
        x += 0.36 / fig_w + t.get_window_extent(fig.canvas.get_renderer()).width / fig_w / fig.dpi + 0.30 / fig_w

    fig.text(0.012, 1 - 0.22 / fig_h,
             "Per file: omnic's automatic picks against every zip tier and 7-Zip's maximum",
             ha="left", va="center", fontsize=13.5, fontweight="bold")
    fig.text(0.012, 1 - 0.54 / fig_h,
             "Silesia, one file at a time. Cell = ratio (share of the original size); the best pick per file is in bold.",
             ha="left", va="center", fontsize=10.5, color=MUTED)
    fig.text(0.012, 0.10 / fig_h,
             "i7-12700, one window, best of 3. Same numbers as the table in BENCHMARK.md.",
             ha="left", va="center", fontsize=8.5, color=MUTED)

    fig.savefig(os.path.join(ASSETS, "per_type_picks.png"), facecolor="white")
    issues = qa(fig, "per_type_picks.png")
    if "--layout" in sys.argv:
        print("    cell luminance (0 = black, 255 = white); 'w' = white ink, 'd' = dark ink")
        for r, e in zip(PICKS_ROWS, excess):
            cells = []
            for x in e:
                _, lum = cell_lum(x)
                cells.append(f"{min(x, VMAX):4.0f}%:{lum:3.0f}{'w' if lum < 118 else 'd'}")
            print(f"      {r[0]:<8} " + "  ".join(cells))
    plt.close(fig)
    return issues


if __name__ == "__main__":
    os.makedirs(ASSETS, exist_ok=True)
    bad = []
    bad += tier_chart(SILESIA, [
        size_panel(1.05) | {"lim": (40, 90), "ticks": [40, 50, 60, 70, 80, 90]},
        pack_panel(1.15) | {"lim": (0.2, 100), "ticks": [0.5, 2, 10, 50]},
        unpack_panel(0.75) | {"lim": (0, 1.6), "ticks": [0, 0.5, 1, 1.5]},
    ])
    bad += tier_chart(GAMETREE, [
        size_panel(1.40) | {"va": 0.50, "lim": (80, 104), "ticks": [80, 85, 90, 95, 100]},
        pack_panel(1.15) | {"lim": (1, 40), "ticks": [1, 2, 5, 10, 20]},
        unpack_panel(0.80) | {"lim": (0, 7.2), "ticks": [0, 2, 4, 6]},
    ])
    bad += picks_heatmap()
    print("assets:", ", ".join(sorted(f for f in os.listdir(ASSETS) if f.endswith(".png"))))
    sys.exit(1 if bad else 0)

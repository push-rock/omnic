#!/usr/bin/env python3
"""omnic advantage plots: product-level charts highlighting measured wins.

Three figures, all from real measurements (i7-12700, single-thread, best of
rounds; protocols documented in README "实测结论"):
  adv_matrix  blind-spot heatmap — every fixed codec has a disaster cell,
              omnic's auto-pick stays within +4.6% of the per-content best
  adv_pack    archive size vs zip (+ single-file extraction panel)
  adv_speed   product-level speed/ratio frontier (per-file CLI protocol)

Usage: python tools/plot_advantage.py   (writes assets/adv_*.{svg,png})
"""
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.colors import TwoSlopeNorm

OMNIC = "#D55E00"   # Okabe-Ito vermillion — omnic accent
GREY = "#9e9e9e"    # competitors
DARK = "#303030"

plt.rcParams.update({
    "figure.dpi": 150, "font.size": 11, "axes.titlesize": 13,
    "axes.titleweight": "bold", "axes.labelsize": 11,
    "xtick.labelsize": 10, "ytick.labelsize": 10,
    "axes.spines.top": False, "axes.spines.right": False,
})


def save(fig, name, footnote):
    fig.text(0.01, 0.012, footnote, fontsize=8, color="#666", va="bottom")
    fig.savefig(f"assets/{name}.svg", bbox_inches="tight")
    fig.savefig(f"assets/{name}.png", dpi=150, bbox_inches="tight")
    plt.close(fig)
    print("wrote", name)


# ---------------------------------------------------------------- adv_matrix
# Volume multiplier vs per-content best (126 MB mixed workspace, 72 files).
# Row best = 1.00; measured sizes: e.g. text gzip 13,786,573 B vs xz 10,201,636 B.
def matrix():
    rows = ["Text ×27\n(35.6 MB)", "SQLite\n(24.1 MB)", "Flat bitmap\n(24.0 MB)",
            "Photo bitmap\n(26.4 MB)", "Fonts/DLL ×7\n(15.7 MB)"]
    cols = ["gzip -9", "zstd -19", "xz -9", "brotli -11", "omnic\n(auto)"]
    m = np.array([
        [1.35, 1.03, 1.00, 1.02, 1.00],
        [1.39, 1.12, 1.00, 1.06, 1.01],
        [37.5, 4.11, 5.72, 1.00, 1.00],
        [1.58, 1.15, 1.00, 1.00, 1.00],
        [1.35, 1.07, 1.00, 1.04, 1.05],
    ])
    fig, ax = plt.subplots(figsize=(9.6, 4.9))
    cmap = plt.get_cmap("RdBu_r")
    im = ax.imshow(m, cmap=cmap, norm=TwoSlopeNorm(vmin=1.0, vcenter=1.35, vmax=6.0), aspect="auto")
    for i in range(m.shape[0]):
        for j in range(m.shape[1]):
            v = m[i, j]
            txt = f"{v:.2f}×" if v < 10 else f"{v:.0f}×"
            bad = v > 1.35  # dark saturated cells get white text
            ax.text(j, i, txt, ha="center", va="center", fontsize=11,
                    fontweight="bold" if j == 4 else "normal",
                    color="white" if bad else DARK)
    ax.set_xticks(range(5), cols)
    ax.set_yticks(range(5), rows)
    for lbl in ax.get_xticklabels():
        if "omnic" in lbl.get_text():
            lbl.set_fontweight("bold"); lbl.set_color(OMNIC)
    ax.axvline(3.5, color="black", lw=2.5)  # separate omnic from the field
    ax.set_title("Every fixed codec has a blind spot — omnic's auto-pick "
                 "stays within +4.6% of the per-content best", pad=12)
    cb = fig.colorbar(im, ax=ax, shrink=0.85, pad=0.02)
    cb.set_label("compressed volume vs per-content best (×)", fontsize=9)
    save(fig, "adv_matrix",
         "126 MB mixed workspace corpus (72 files) · cell = compressed volume as a multiple of that "
         "row's best codec · 37.5× is off-scale (gzip on flat bitmaps) · i7-12700, single thread")


# ------------------------------------------------------------------ adv_pack
# 152.6 MB Luanti game tree (8,586 files); sizes are measured archive bytes/2^20.
def pack():
    fig, (a, b) = plt.subplots(1, 2, figsize=(12.5, 4.6))
    # Panel A: archive size
    names = ["omnic --no-skip", "omnic -p best_ratio", "omnic (solid, default)",
             "omnic --per-file", "zip -9", "zip (default)"]
    sizes = [90.5, 92.0, 95.7, 99.6, 100.7, 100.8]
    delta = ["−10.2%", "−8.7%", "−5.0%", "−1.2%", "−0.1%", "—"]
    colors = [OMNIC, OMNIC, OMNIC, OMNIC, GREY, GREY]
    y = np.arange(len(names))
    a.barh(y, sizes, color=colors, height=0.62)
    a.set_yticks(y, names)
    for lbl in a.get_yticklabels():
        lbl.set_fontweight("bold" if "omnic" in lbl.get_text() else "normal")
    a.axvline(100.8, color=GREY, ls="--", lw=1.2)
    a.text(100.9, -0.55, "zip", fontsize=9, color="#666")
    for i, (s, d) in enumerate(zip(sizes, delta)):
        a.text(s + 1.0, i, f"{s:.1f} MB   {d} vs zip", va="center", fontsize=10)
    a.set_xlim(0, 122)
    a.invert_yaxis()
    a.set_xlabel("archive size (MB) — smaller is better")
    a.set_title("omnic archive is 5% smaller than zip\n(10.2% smaller with --no-skip)")
    # Panel B: single-file extraction (random access, 6 KB file out of 8,586 entries)
    enames = ["omnic --per-file", "zip (.NET library)", "zip (bsdtar CLI)"]
    ems = [8.7, 25.0, 28.3]
    ecol = [OMNIC, GREY, GREY]
    y2 = np.arange(len(enames))
    b.barh(y2, ems, color=ecol, height=0.56)
    b.set_yticks(y2, enames)
    for lbl in b.get_yticklabels():
        lbl.set_fontweight("bold" if "omnic" in lbl.get_text() else "normal")
    notes = ["8.7 ms — 3.3× faster than zip\n(timing includes process start)",
             "25 ms in-process\n(≈20 ms is parsing 8,586 central-dir\nentries before any data is read)",
             "28.3 ms (CLI)"]
    for i, (v, t) in enumerate(zip(ems, notes)):
        b.text(v + 1.2, i, t, va="center", fontsize=9)
    b.set_xlim(0, 58)
    b.invert_yaxis()
    b.set_xlabel("extract one 6 KB file (ms) — smaller is better")
    b.set_title("random access: one file out of 8,586\nwithout unpacking — 3.3× faster than zip")
    fig.tight_layout()
    save(fig, "adv_pack",
         "152.6 MB Luanti game tree (8,586 files) · zip = libarchive deflate · extraction best of 5 "
         "rounds · solid mode trades extraction (67 ms: decodes one whole 64 MiB block) for better "
         "ratio — see left panel · i7-12700, single thread")


# ----------------------------------------------------------------- adv_speed
# Per-file CLI protocol on the mixed workspace corpus (126 MB, 72 files).
def speed():
    fig, ax = plt.subplots(figsize=(9.0, 5.6))
    pts = [  # name, MiB/s, ratio %, omnic?
        ("omnic best_ratio", 3.0, 19.2, True),
        ("zstd -19", 3.2, 20.4, False),
        ("omnic balanced", 85.0, 24.3, True),
        ("gzip -9", 38.0, 26.4, False),
        ("zstd -3 (CLI default)", 261.0, 26.5, False),
    ]
    for name, cs, r, is_omnic in pts:
        ax.scatter(cs, r, s=170 if is_omnic else 130,
                   color=OMNIC if is_omnic else GREY, zorder=3,
                   marker="o" if is_omnic else "s", edgecolor="white", linewidth=1.2)
    # frontier: BR → balanced → zstd -3 (omnic owns it except the extreme-speed corner)
    fx, fy = [3.0, 85.0, 261.0], [19.2, 24.3, 26.5]
    ax.plot(fx, fy, ls="--", color=OMNIC, lw=1.8, zorder=2, alpha=0.8)
    ann = {
        "omnic best_ratio": (3.0, 19.2, (-8, 14), "left"),
        "omnic balanced": (85.0, 24.3, (10, -18), "left"),
        "zstd -3 (CLI default)": (261.0, 26.5, (-6, -22), "right"),
        "gzip -9": (38.0, 26.4, (6, 10), "left"),
        "zstd -19": (3.2, 20.4, (8, -26), "left"),
    }
    for name, (x, yv, (dx, dy), ha) in ann.items():
        dominated = name in ("gzip -9", "zstd -19")
        label = name + ("\n(strictly dominated)" if dominated else "")
        ax.annotate(label, (x, yv), textcoords="offset points", xytext=(dx, dy),
                    ha=ha, fontsize=9.5,
                    color=OMNIC if "omnic" in name else "#555",
                    fontweight="bold" if "omnic" in name else "normal")
    ax.set_xscale("log")
    ax.set_xlim(1.8, 700)
    ax.set_ylim(27.6, 18.4)  # inverted: up = smaller archive = better
    ax.set_xlabel("compress throughput (MiB/s, log scale) — right is better")
    ax.set_ylabel("compressed size / original (%) — up is better")
    ax.set_title("omnic owns the compression frontier — same speed as zstd -19 with 6.2% "
                 "smaller output; 2.2× faster than gzip -9 and smaller", pad=12)
    ax.grid(True, alpha=0.3)
    save(fig, "adv_speed",
         "126 MB mixed workspace corpus (72 files), per-file CLI protocol · dashed line = Pareto "
         "frontier (best ratio at each speed) · dominated = some other point is faster AND smaller "
         "· i7-12700, single thread")


if __name__ == "__main__":
    matrix()
    pack()
    speed()

#!/usr/bin/env python3
"""omnic benchmark plot: speed/ratio Pareto chart from omnic_bench CSV.

Usage:
    python plot_bench.py bench32.csv -o assets/bench --footnote "CPU ..."

Reads the CSV emitted by `omnic_bench --csv` (columns: codec,format,level,
ratio_pct,comp_base_mbs,comp_adapt_mbs,comp_loss_pct,dec_base_mbs,
dec_adapt_mbs,dec_loss_pct) and renders a two-panel figure:
  left : compression speed (MB/s, log) vs space savings (%)
  right: decompression speed (MB/s, log) vs space savings (%)
Each codec is one curve through its levels; the representative level is
highlighted. Output: <out>.svg and <out>.png.
"""
import argparse
import csv
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

# Representative level per codec (highlighted marker + label anchor)
DEFAULT_LEVEL = {
    "zstd": 6,
    "lz4": 3,
    "gzip": 6,
    "zlib": 6,
    "raw-deflate": 6,
    "zlib-ng": 6,
    "libdeflate": 6,
    "brotli": 9,
    "xz": 6,
    "lzma": 6,
    "snappy": 1,
    "lzfse": 0,
    "bzip2": 9,
    "lzip": 6,
    "compress": 9,
    "lzvn": 0,
}

# Display order: fast-general-purpose first, then high-ratio, then niche
CODEC_ORDER = [
    "lz4", "snappy", "lzfse", "zlib-ng", "gzip", "zlib", "raw-deflate",
    "libdeflate", "zstd", "brotli", "xz", "lzma", "bzip2", "lzip", "lzvn",
    "compress",
]

MARKERS = ["o", "s", "^", "D", "v", "P", "X", "*", "<", ">", "p", "h", "d", "H"]


def load(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            try:
                rows.append({
                    "codec": r["codec"],
                    "level": int(r["level"]),
                    "ratio": float(r["ratio_pct"]),
                    "cspeed": float(r["comp_adapt_mbs"]),
                    "dspeed": float(r["dec_adapt_mbs"]),
                    "closs": float(r["comp_loss_pct"]),
                    "dloss": float(r["dec_loss_pct"]),
                })
            except (KeyError, ValueError):
                continue
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", help="omnic_bench --csv output")
    ap.add_argument("-o", "--out", default="bench", help="output basename")
    ap.add_argument("--footnote", default="", help="environment note (CPU/compiler/date)")
    args = ap.parse_args()

    rows = load(args.csv)
    if not rows:
        sys.exit("no data rows in " + args.csv)

    by_codec = defaultdict(list)
    for r in rows:
        by_codec[r["codec"]].append(r)
    for v in by_codec.values():
        v.sort(key=lambda r: r["cspeed"])

    codecs = [c for c in CODEC_ORDER if c in by_codec]
    codecs += [c for c in by_codec if c not in CODEC_ORDER]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13.2, 5.6), sharey=True)
    fig.patch.set_facecolor("white")
    cmap = plt.get_cmap("tab20")

    for i, c in enumerate(codecs):
        pts = by_codec[c]
        x1 = [p["cspeed"] for p in pts]
        x2 = [p["dspeed"] for p in pts]
        y = [100.0 - p["ratio"] for p in pts]
        color = cmap(i % 20)  # 20 色：12 codec 不复用
        marker = MARKERS[i % len(MARKERS)]
        for ax, x in ((ax1, x1), (ax2, x2)):
            ax.plot(x, y, "-", color=color, alpha=0.75, lw=1.6, zorder=2)
            ax.plot(x, y, marker, color=color, ms=5.5, mec="white", mew=0.6, zorder=3,
                    ls="none", label=c if ax is ax1 else None)
        # representative level: enlarged ringed marker + text
        want = DEFAULT_LEVEL.get(c, pts[len(pts) // 2]["level"])
        for p, xx1, xx2 in zip(pts, x1, x2):
            if p["level"] != want:
                continue
            for ax, xx in ((ax1, xx1), (ax2, xx2)):
                ax.plot(xx, 100.0 - p["ratio"], "o", ms=13, mfc="none", mec=color,
                        mew=1.4, zorder=4)
        p0 = min(pts, key=lambda p: abs(p["level"] - want))
        off = (8, 7) if i % 2 == 0 else (8, -15)  # 交替上下错位，低速密集区防重叠
        ax1.annotate(c, (p0["cspeed"], 100.0 - p0["ratio"]),
                     textcoords="offset points", xytext=off,
                     fontsize=9, color=color, fontweight="bold")
        ax2.annotate(c, (p0["dspeed"], 100.0 - p0["ratio"]),
                     textcoords="offset points", xytext=off,
                     fontsize=9, color=color, fontweight="bold")

    for ax, title in ((ax1, "Compression"), (ax2, "Decompression")):
        ax.set_xscale("log")
        ax.set_xlabel(f"{title} speed (MB/s, omnic adapter path)")
        ax.set_title(f"{title}: speed vs ratio", fontsize=12, fontweight="bold")
        ax.grid(True, which="both", ls=":", lw=0.5, alpha=0.55)
        ax.set_axisbelow(True)
        for s in ("top", "right"):
            ax.spines[s].set_visible(False)
        ax.tick_params(labelsize=9)
    ax1.set_ylabel("Space savings (% of original)")

    xmax1 = max(r["cspeed"] for r in rows)
    xmax2 = max(r["dspeed"] for r in rows)
    ax1.set_xlim(min(x for x in x1 if x > 0) / 1.6 if x1 else 1, xmax1 * 2.2)
    ax2.set_xlim(min(r["dspeed"] for r in rows if r["dspeed"] > 0) / 1.6, xmax2 * 2.2)

    fig.suptitle("omnic codec landscape", fontsize=14, fontweight="bold", y=0.98)
    note = args.footnote or "measured with omnic_bench (32 MiB mixed corpus, median of 3)"
    fig.text(0.99, 0.01, note, ha="right", fontsize=7.5, color="#666666")
    fig.tight_layout(rect=(0, 0.03, 1, 0.95))

    for ext in ("svg", "png"):
        out = f"{args.out}.{ext}"
        fig.savefig(out, dpi=160, bbox_inches="tight")
        print("wrote", out)


if __name__ == "__main__":
    main()

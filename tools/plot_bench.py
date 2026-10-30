#!/usr/bin/env python3
"""omnic benchmark plot: speed/ratio landscape from omnic_bench CSV.

Usage:
    python plot_bench.py bench32.csv -o assets/bench --footnote "..."

Design follows the data-visualization skill: <= 8 hue groups (format families
share a hue, differ by shade/linestyle), labels are de-overlapped with
adjustText, consistent y-scale across panels, insight-style titles.
Output: <out>.svg and <out>.png.
"""
import argparse
import csv
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from adjustText import adjust_text

# Format families share a hue; shade + linestyle separates members.
# Colorblind-safe base hues (Okabe-Ito derived), 7 groups total.
FAMILY = {
    # deflate family: three shades of purple
    "gzip":        ("#7b68ae", "-",  "gzip (zlib)"),
    "zlib-ng":     ("#a99bd4", "--", "zlib-ng"),
    "libdeflate":  ("#4a3b78", "-.", "libdeflate"),
    # one hue per remaining engine
    "zstd":        ("#3d6dd1", "-",  "zstd"),
    "lz4":         ("#3f9c5a", "-",  "lz4"),
    "brotli":      ("#d1483c", "-",  "brotli"),
    "xz":          ("#e08a2e", "-",  "xz"),
    "snappy":      ("#8a6d3b", "-",  "snappy"),
    "lzfse":       ("#c96fa8", "-",  "lzfse"),
}
MERGED = {"zlib": "gzip", "raw-deflate": "gzip", "lzma": "xz"}  # same engine, identical numbers
DEFAULT_LEVEL = {
    "zstd": 6, "lz4": 3, "gzip": 6, "zlib-ng": 6, "libdeflate": 6, "brotli": 9,
    "xz": 6, "snappy": 1, "lzfse": 0,
}
ORDER = ["lz4", "snappy", "lzfse", "zlib-ng", "gzip", "libdeflate", "zstd",
         "brotli", "xz"]


def load(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            try:
                c = MERGED.get(r["codec"], r["codec"])
                if c not in FAMILY:
                    continue
                rows.append({
                    "codec": c,
                    "level": int(r["level"]),
                    "ratio": float(r["ratio_pct"]),
                    "cspeed": float(r["comp_adapt_mbs"]),
                    "dspeed": float(r["dec_adapt_mbs"]),
                })
            except (KeyError, ValueError):
                continue
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("-o", "--out", default="bench")
    ap.add_argument("--footnote", default="")
    args = ap.parse_args()

    rows = load(args.csv)
    if not rows:
        sys.exit("no plottable rows in " + args.csv)

    by_codec = defaultdict(list)
    for r in rows:
        by_codec[r["codec"]].append(r)
    for v in by_codec.values():
        v.sort(key=lambda r: r["cspeed"])
    codecs = [c for c in ORDER if c in by_codec]

    plt.style.use("seaborn-v0_8-whitegrid")
    plt.rcParams.update({
        "font.size": 11, "axes.titlesize": 13, "axes.titleweight": "bold",
        "axes.labelsize": 11, "legend.fontsize": 10, "figure.titlesize": 15,
    })

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6), sharey=True)
    fig.patch.set_facecolor("white")

    # 面板一：压缩——level 曲线展示 speed/ratio 全谱
    texts1 = []
    for c in codecs:
        color, ls, label = FAMILY[c]
        pts = by_codec[c]
        x = [p["cspeed"] for p in pts]
        y = [100.0 / p["ratio"] for p in pts]  # ratio = original / compressed
        ax1.plot(x, y, ls, color=color, alpha=0.55, lw=1.5, zorder=2)
        ax1.plot(x, y, "o", color=color, ms=4.5, mec="white", mew=0.5,
                 zorder=3, ls="none")
        want = DEFAULT_LEVEL.get(c, pts[len(pts) // 2]["level"])
        p0 = min(pts, key=lambda p: abs(p["level"] - want))
        ax1.plot(p0["cspeed"], 100.0 / p0["ratio"], "o", ms=15, mfc="none",
                 mec=color, mew=1.6, zorder=4)
        texts1.append(ax1.text(p0["cspeed"], 100.0 / p0["ratio"], label,
                               color=color, fontsize=10, fontweight="bold", zorder=5))
    ax1.set_xscale("log")
    ax1.set_xlabel("Compression speed through omnic (MB/s)")
    ax1.set_title("Compression levels: the speed/ratio frontier",
                  fontsize=12, fontweight="bold")
    for s in ("top", "right"):
        ax1.spines[s].set_visible(False)
    adjust_text(texts1, ax=ax1, expand=(1.4, 1.8), only_move={"text": "xy"},
                min_arrow_len=8, arrowprops=dict(arrowstyle="-", color="#999999", lw=0.7))

    # 面板二：解压——速度与 level 基本无关，画 level 曲线只会挤成竖线团；
    # 每 codec 一个代表点才如实呈现
    texts2 = []
    for c in codecs:
        color, _, label = FAMILY[c]
        pts = by_codec[c]
        want = DEFAULT_LEVEL.get(c, pts[len(pts) // 2]["level"])
        p0 = min(pts, key=lambda p: abs(p["level"] - want))
        ax2.plot(p0["dspeed"], 100.0 / p0["ratio"], "o", color=color, ms=9,
                 mec="white", mew=0.8, zorder=3)
        texts2.append(ax2.text(p0["dspeed"], 100.0 / p0["ratio"], label,
                               color=color, fontsize=10, fontweight="bold", zorder=5))
    ax2.set_xscale("log")
    ax2.set_xlabel("Decompression speed through omnic (MB/s)")
    ax2.set_title("Decompression: one point per codec (level barely matters)",
                  fontsize=12, fontweight="bold")
    for s in ("top", "right"):
        ax2.spines[s].set_visible(False)
    adjust_text(texts2, ax=ax2, expand=(1.4, 1.8), only_move={"text": "xy"},
                min_arrow_len=8, arrowprops=dict(arrowstyle="-", color="#999999", lw=0.7))

    ax1.set_ylabel("Compression ratio (original : compressed)")
    xmax = max(max(p["cspeed"] for p in v) for v in by_codec.values())
    xmax2 = max(max(p["dspeed"] for p in v) for v in by_codec.values())
    ax1.set_xlim(2, xmax * 2.5)
    ax2.set_xlim(60, xmax2 * 2.5)
    ymin = min(100.0 / max(p["ratio"] for p in v) for v in by_codec.values())
    ymax = max(100.0 / min(p["ratio"] for p in v) for v in by_codec.values())
    ax1.set_ylim(ymin - 0.25, ymax + 0.4)

    fig.suptitle("omnic codec landscape", fontweight="bold", y=0.99)
    note = args.footnote or "32 MiB mixed corpus, median of 3, omnic_bench"
    fig.text(0.99, 0.005, note, ha="right", fontsize=7.5, color="#666666")
    fig.text(0.01, 0.005,
             "Ratio = original : compressed (higher is smaller output). Ringed marker = representative level. "
             "zlib/raw-deflate/lzma omitted: same engines as gzip/xz.",
             ha="left", fontsize=7.5, color="#666666")
    fig.tight_layout(rect=(0, 0.04, 1, 0.96))

    for ext in ("svg", "png"):
        out = f"{args.out}.{ext}"
        fig.savefig(out, dpi=170, bbox_inches="tight")
        print("wrote", out)


if __name__ == "__main__":
    main()

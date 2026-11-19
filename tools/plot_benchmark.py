#!/usr/bin/env python3
"""Benchmark hero figures for the public BENCHMARK.md page and social posts.

Two figures, all numbers straight from the README tables (same-tree,
same-window, best-of-3 measurements — zero drift with the README):

  bench_hero    game tree vs zip: size / pack time / single-file extract
  bench_silesia Silesia vs 7-Zip: the max-tier upset (ratio ties, speed wins)

Usage: python tools/plot_benchmark.py   (writes assets/bench_*.{svg,png})
"""
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

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


def bar_panel(ax, labels, values, texts, colors, xlabel, best_side="low"):
    y = range(len(labels))
    ax.barh(y, values, color=colors, height=0.62, zorder=3)
    ax.set_yticks(y, labels)
    for lbl in ax.get_yticklabels():
        if "omnic" in lbl.get_text():
            lbl.set_fontweight("bold")
            lbl.set_color(OMNIC)
    ax.invert_yaxis()
    vmax = max(values)
    ax.set_xlim(0, vmax * 1.22)
    for i, (v, t) in enumerate(zip(values, texts)):
        ax.text(v + vmax * 0.015, i, t, va="center", fontsize=10, color=DARK)
    ax.set_xlabel(xlabel, fontsize=10)
    ax.grid(axis="x", color="#e6e6e6", zorder=0)


# ------------------------------------------------------------ bench_hero
# README "Archiving" table: same 152.6 MB game tree, 8,586 files.
def hero():
    rows = [
        # label, size MB, pack s, extract ms, omnic?
        ("zip (deflate default)",      100.8, 3.8, 28.3, False),
        ("zip -9 (deflate max)",       100.7, 7.9, 28.3, False),
        ("omnic (solid, default)",      95.7, 1.9, 67.0, True),
        ("omnic --per-file",            99.6, 1.7,  8.7, True),
        ("omnic --per-file -p best_ratio", 96.7, 6.0, 8.7, True),
        ("omnic -p best_ratio",         92.0, 11.5, 67.0, True),
        ("omnic --no-skip",             90.5, 12.7, 67.0, True),
    ]
    labels = [r[0] for r in rows]
    sizes = [r[1] for r in rows]
    packs = [r[2] for r in rows]
    extracts = [r[3] for r in rows]
    colors = [OMNIC if r[4] else GREY for r in rows]

    fig, axes = plt.subplots(1, 3, figsize=(14.5, 4.6),
                             gridspec_kw={"width_ratios": [2.6, 2.2, 2.2]})
    fig.subplots_adjust(wspace=0.42)

    bar_panel(axes[0], labels, sizes, [f"{s:.1f} MB" for s in sizes], colors,
              "archive size — smaller is better")
    axes[0].axvline(100.8, color=GREY, lw=1.2, ls="--", zorder=2)
    axes[0].text(100.8, -0.62, "zip", fontsize=9, color="#666", ha="center")

    bar_panel(axes[1], labels, packs, [f"{p:.1f} s" for p in packs], colors,
              "pack time — lower is better")
    axes[1].axvline(3.8, color=GREY, lw=1.2, ls="--", zorder=2)
    axes[1].text(3.8, -0.62, "zip", fontsize=9, color="#666", ha="center")

    bar_panel(axes[2], labels, extracts, [f"{e:.1f} ms" for e in extracts],
              colors, "single-file extract — lower is better")
    axes[2].axvline(28.3, color=GREY, lw=1.2, ls="--", zorder=2)
    axes[2].text(28.3, -0.62, "zip", fontsize=9, color="#666", ha="center")
    # the headline: 8.7 ms = 3.3x faster than zip CLI
    axes[2].annotate("3.3× faster than zip CLI", xy=(8.7, 3.35),
                     xytext=(26, 2.4), fontsize=10, fontweight="bold",
                     color=OMNIC, arrowprops=dict(arrowstyle="->", color=OMNIC))

    axes[0].set_title("omnic vs zip — same tree, three axes", loc="left")
    save(fig, "bench_hero",
         "152.6 MB game tree, 8,586 files (PNG/Lua/Ogg/fonts/DLL) · best of 3 on i7-12700 · "
         "single-file extract = one 6 KB Lua file out of 8,586 entries, process start included · "
         "solid modes pay 67 ms to read+decode one 64 MiB block · full tables: README / BENCHMARK.md")


# -------------------------------------------------------- bench_silesia
# README "7-Zip cross-check" table: Silesia, same machine and window.
def silesia():
    rows = [
        ("omnic default (balanced)", 28.97, 1.06, True),
        ("7z -mx=5 (its default)",   23.40, 24.45, False),
        ("omnic -p best_ratio",      23.02, 30.2, True),
        ("7z -mx=9 (its max)",       22.99, 50.8, False),
    ]
    labels = [r[0] for r in rows]
    ratios = [r[1] for r in rows]
    packs = [r[2] for r in rows]
    colors = [OMNIC if r[3] else GREY for r in rows]

    fig, axes = plt.subplots(1, 2, figsize=(11.8, 3.6),
                             gridspec_kw={"width_ratios": [1.6, 1.8]})
    fig.subplots_adjust(wspace=0.35)

    bar_panel(axes[0], labels, ratios, [f"{r:.2f}%" for r in ratios], colors,
              "ratio — smaller is better")
    axes[0].axvline(22.99, color=GREY, lw=1.2, ls="--", zorder=2)
    axes[0].text(22.99, -0.62, "7z max", fontsize=9, color="#666", ha="center")

    bar_panel(axes[1], labels, packs, [f"{p:.1f} s" for p in packs], colors,
              "pack time — lower is better")
    axes[1].axvline(50.8, color=GREY, lw=1.2, ls="--", zorder=2)
    axes[1].text(50.8, -0.62, "7z max", fontsize=9, color="#666", ha="center")

    axes[0].set_title("Silesia (202 MiB): max tiers tie on ratio — "
                      "omnic packs it 1.7× faster", loc="left")
    save(fig, "bench_silesia",
         "Silesia corpus, 12 domain-mixed files (202 MiB) · 7zr 26.06, LZMA2 · "
         "same machine and window, best of rounds · omnic tiers are per-file routed; "
         "7z compresses one solid stream")


if __name__ == "__main__":
    hero()
    silesia()

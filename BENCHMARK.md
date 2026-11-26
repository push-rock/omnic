# omnic benchmark

All numbers are measured, same machine (i7-12700, Windows, NVMe SSD), same
window — omnic, 7-Zip 26.06 and the system tar (bsdtar) interleaved in one
session — and identical to the tables in the [README](README.md). This page
is the one-glance version; methodology at the bottom.

![Archive Pareto: the omnic ladder vs zip and 7-Zip](assets/archive_ladder.png)

**The same 152.6 MB game tree (8,586 files — PNG textures, Lua scripts, Ogg
audio, models, fonts, DLLs), every zip tier:**

| option | ratio | archive size | vs zip | pack | single-file extract |
|---|---|---|---|---|---|
| zip (deflate default) | 69.20% | 100.7 MB | — | 3.86 s | 26.9 ms |
| zip -9 (deflate max, 7z encoder) | 67.97% | 99.0 MB | -1.8% | 6.35 s | 26.2 ms |
| **omnic pack** (solid, default) | **65.77%** | **95.7 MB** | **-5.0%** | **1.55 s** | 54 ms |
| omnic pack --per-file | 68.43% | 99.6 MB | -1.1% | 1.47 s | **8.8 ms** |
| omnic pack --per-file -p best_ratio | 66.44% | 96.7 MB | -4.0% | 5.9 s | **8.9 ms** |
| omnic pack -p best_ratio | 63.22% | 92.0 MB | -8.6% | 11.3 s | 201 ms |
| omnic pack --no-skip | **62.17%** | **90.5 MB** | **-10.2%** | 12.5 s | 54 ms |

- **Smaller and faster at the default tier**: -5.0% size at 40% of the pack time.
- **Single-file extraction is the per-file tier's game**: 8.8 ms (process start
  included) vs 26.9 ms for the fastest zip tool we could find (bsdtar) — 3×.
  The honest cost of solid mode: extracting from an xz block decodes the whole
  64 MiB block (201 ms for best_ratio); zip seeks instead. Use `--per-file`
  when member latency matters.
- **The ratio ceiling**: --no-skip at -10.2% vs zip, where deflate simply
  cannot follow (it caps at 68.0% even at its max).

## vs 7-Zip — the max tier (same window, 7z 26.06)

Silesia (the standard 202 MiB, 12-domain corpus — novels, dictionary,
chemistry database, C source, medical/X-ray images, binaries, XML; no file
extensions, so every decision goes through content sampling):

| option | ratio | pack |
|---|---|---|
| **omnic default (balanced)** | 28.97% | **0.76 s** |
| 7z -mx=5 (its default) | 23.40% | 21.6 s |
| **omnic -p best_ratio** | 23.02% | **30.2 s** |
| 7z -mx=9 (its max) | **22.97%** | 46.3 s |

Default tiers are different animals (omnic's is a speed tier, 28× faster and
5.6 pp larger). At the max tier they tie on ratio to 0.05 pp — and omnic is
the faster one by 1.5×: block compression runs through a worker pool while a
single 7z stream serializes LZMA2.

Cross-corpus vs zip on Silesia: balanced is 3.2 pp smaller, 6.6× faster to
pack, and 1.9× faster to unpack (28.97% / 0.76 s vs 32.18% / 4.99 s;
unpack 0.20 s vs 0.38 s).

## What's measured, and what it costs

Recorded losses, same protocol: full unpack of the game tree is 3.2 s vs 1.8 s
for the signed system tar on the same zip content — Windows Defender taxes
every file an unsigned process creates (a bare robocopy of the same tree
costs 3.7 s), and the pipeline is now fast enough to hit that per-file scan
wall head-on; block decode+verify without writes is 0.12 s, so the
per-block CRC that buys corruption isolation is not the bottleneck. Exclude
the output directory from Defender and omnic's unpack drops to ~1.8 s — on
par with tar. On the big-file Silesia corpus the former recorded loss
(0.83 s vs zip's 0.37 s) flipped twice — slice-by-8 CRC-32, then the
write-pool gate learning to admit big-file shapes (12 entries, 202 MiB) so
writes overlap the decode: full unpack is now **0.20 s vs 0.38 s**. Details
and the honest-losses notes live in the
[README's measured section](README.md#archiving-two-corpora-every-tier).

Corruption isolation, for the record: every block carries a CRC-32; one
flipped bit kills one block and names the affected files while the rest of the
tree extracts cleanly (`extracted 7435/8586 files`, exit code 2). A zip with a
damaged central directory usually gives you nothing.

## Methodology

- **Corpora**: a real game tree (Luanti 5.16 + minetest_game + VoxeLibre,
  152.6 MB, 8,586 files) and the standard Silesia corpus (202 MiB, 12 files).
- **Timing**: wall clock of the whole CLI invocation, best of 3 (pack) or
  7 (millisecond-scale extraction), on an idle machine; every competitor row
  is measured in the same window as its omnic counterpart (7-Zip 26.06, bsdtar
  zip/gzip, codec-native levels through omnic's vendored libraries).
- **Protocols**: archive rows are directory-to-archive; the whole-stream
  table (README) follows the lzbench / Oodle published protocol. Nothing is
  extrapolated — if a cell is not measured, it is not printed.
- **Reproduce**: corpora described above, `omnic pack <dir> -o out.omnic`
  vs `tar -a -c -f out.zip <dir>` (bsdtar deflate) and `7z a -mx=9 out.7z`.

## Install

```
scoop bucket add omnic https://github.com/push-rock/omnic-bucket
scoop install omnic
```

or grab a binary from [Releases](https://github.com/push-rock/omnic/releases)
(Windows / Linux / macOS), or build from source — it's CMake + C11.

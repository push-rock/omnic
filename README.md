# omnic

**Universal compressor** — one binary that picks the right codec for each file, and decompresses most formats you'll meet in the wild.

[English](README.md) | [简体中文](README.zh-CN.md)

```
$ omnic c report.json
report.json: zstd | balanced | text | level 8
    1048576 -> 91234 bytes (8.70%), 0.091s, 11.5 MB/s

$ omnic c --profile best_ratio report.json
report.json: brotli | best_ratio | text | level 11
    1048576 -> 61042 bytes (5.82%), 0.322s, 3.3 MB/s

$ omnic d report.json.xz
report.json.xz: xz
    61042 -> 1048576 bytes, 0.019s, 55.2 MB/s
```

## Why

lzbench benchmarks 50+ compressors, but no mainstream tool routes between them. `zstd --adapt` adapts levels *within* one codec; 7-Zip doesn't care whether the input is JSON or a bitmap. omnic closes that gap:

- **Compress**: classify the input (extension routing + 3-segment sample: 256 KiB head/middle/tail, 8 features), then look up a policy table — *8 data classes × 4 preference profiles* — to pick `(codec, level)`. A full decision takes about a millisecond, most of it I/O.
- **Prefer**: `--profile fast_compress | fast_decompress | best_ratio | balanced`.
- **Decompress**: recognize the format by magic number first, extension as fallback. Native formats only, so every output can be unpacked by the official tools of that codec.
- **Skip incompressible data**: already-compressed inputs (jpeg/zip/…) and high-entropy data (≥7.5 bits/byte) are skipped with a warning; `--force` overrides.

## Build

Requires CMake ≥ 3.16 and a C11 compiler (MSVC / GCC / Clang). All codecs are vendored — no system dependencies.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/omnic list
ctest --test-dir build
```

## Usage

```
omnic c   [options] <file...>    compress (auto codec unless -c)
omnic d   [options] <file...>    decompress (auto format by magic/ext)
omnic analyze <file...>          show type/entropy/decision, no side effects
omnic list                       list codecs and recognized formats

-p --profile <v>   fast_compress | fast_decompress | best_ratio | balanced
-c --codec <name>  force a codec            -l --level <n>  codec-native level
-T --threads <n>   worker threads (zstd)    -o --output-dir <dir>
-f --force    overwrite output / compress anyway
-k --keep     keep the source file (default: removed after success)
-x --explain  print the decision rationale  -q --quiet
```

## Codecs

| codec | format | levels | notes |
|---|---|---|---|
| zstd 1.5.7 | .zst | -5..22 | multithreaded |
| lz4 1.10.0 | .lz4 | 0..12 | level 0 = fast mode, 1..12 = LZ4HC |
| xz 5.8.4 | .xz | 0..9 | CRC64 |
| zlib 1.3.2 | .gz | 0..9 | streaming, multi-member gzip on decompress |
| zlib-ng 2.3.3 | .gz | 0..9 | same format, faster implementation |
| libdeflate 1.26 | .gz | 0..12 | whole-buffer API, highest gzip speed |
| brotli 1.2.1 | .br | 0..11 | |
| snappy 1.3.1 | .snappy | 1..2 | official framing format (magic + CRC32C + 64 KB chunks) |
| lzfse 1.0 | .lzfse | — | single speed point |
| zlib-stream | .zlib | 0..9 | RFC 1950 wrapper, same zlib library |
| raw-deflate | .zz | 0..9 | headerless deflate, recognized by extension; no checksum — corrupted streams can decode silently (format-inherent) |
| lzma | .lzma | 0..9 | lzma-alone container, via liblzma |
| lzvn | — | decode only | "bvn" streams, shares the lzfse block decoder |
| lzip | .lz | decode only | lzip container (v0/v1), via liblzma, multi-member |
| bzip2 1.0.8 | .bz2 | decode only | vendored decode half, concatenated streams |
| compress | .Z | decode only | LZW, ncompress semantics, self-contained decoder |

All adapters share one interface (`src/codec/ac_*.c`), so adding a codec is a single file plus a registry entry. Decompression covers all 14 formats: gzip (multi-member), zlib, raw deflate, zstd, xz, lzma, lzip, bzip2, lz4, brotli, snappy, compress (.Z), lzfse, lzvn — recognized by magic number first, extension as fallback.

## Performance

![codec landscape](assets/bench.svg)

Speed vs ratio for the compressors, measured through omnic's streaming adapter layer on the [Silesia corpus](https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia) (212 MiB, 12 real-world files; fastest of 3 rounds). Reproduce with the bundled tool:

```
omnic_bench --rounds 3 --csv bench.csv --corpus silesia/dickens --corpus silesia/mozilla ... (12 files)
python tools/plot_bench.py bench.csv -o assets/bench
```

`omnic_bench` also measures adapter overhead against direct library calls on the same data: on Silesia, every codec stays within **0.8% on compression and 1.4% on decompression** (reference: i7-12700, GCC 16.2 MinGW-w64; raw numbers in `assets/bench.csv`). Most decompression paths are actually *faster* through omnic (up to +60%: zstd low levels, lz4, snappy): fixed-size chunk buffers stay cache-hot, while one-shot baselines stream through buffers larger than L3. That margin is not free — it comes from seek-based size preallocation for whole-buffer codecs, 1 MiB stdio buffering for small-chunk formats, hardware CRC32C (SSE4.2/ARM64) in the snappy framing, and a block-header pre-scan that sizes lzfse output exactly. `ctest` runs a 40% adapter-overhead smoke gate (`bench_smoke`, synthetic corpus — self-contained and reproducible in CI) on every push; the tight thresholds are enforced by local runs of the same tool.

## How it picks

The decision pipeline runs in about **1 ms** (dominated by I/O: three 256 KiB segments sampled from head/middle/tail — the first-order entropy is already converged at that size, so a 4 GB file costs no more than a 4 MB one):

1. **Extension routing** maps known extensions to a coarse class (unknown extensions fall back to content).
2. **8 features** (entropy, printable ratio, 16 B / 4 KiB window duplicate rates, byte-pair conditional entropy, block-entropy CV, delta entropy…) refine the coarse class into one of **7 content classes** — `natural_text`, `source_code`, `structured`, `graphic`, `synthetic_pic` (flat synthetic graphics: icons/charts/screenshots), `photo`, `executable` — with `already_compressed` / `random` as skip short-circuits.
3. **Policy table lookup** returns `(codec, level)` per profile. For large files the `fast_compress` profile trial-compresses its top-2 candidates on a 256 KiB mid-segment under a 50 ms budget before committing.
4. `--explain` prints the whole chain: class, features, and why that row of the table fired.

The table itself is **measured, not hand-tuned**: `tools/train/` holds the full pipeline (bench → fit → gen). The shipped table is fit from a 22-distribution × 44-config benchmark matrix, and every release is gated by:

- **Leave-one-out regret bound** — refit the table with each distribution held out; the holdout's regret against the oracle pick must stay within ρ ≤ 1.0 pp (ratio) and v ≤ 50% (speed).
- **External validation** on real files never used in training — the fitted table beats the v0.1.0 hand-tuned one at near-zero regret on text/photo/executable.
- **Survivorship-bias control** — fitted ρ-regret 0.31 pp vs hand-tuned 0.45 pp vs random 2.33 pp.

The failure surface is documented rather than hidden: 5 of 22 distributions exceed the regret bound, all traced to intra-class polymorphism (real photos vs themed bitmaps, 64- vs 32-bit binaries). Adding more corpora was measured and does *not* fix it — the fix is a finer class split, listed as future work.

## Real-world: a game asset tree

One 152.5 MB real game tree (Luanti 5.16 + minetest_game + VoxeLibre; 8,586 files — PNG textures, Lua scripts, Ogg sounds, models, fonts, DLLs). All numbers same machine, single thread (i7-12700).

**Packed as one stream** — archive-then-compress, the protocol external benchmarks (lzbench, Oodle's published figures) use:

| configuration | ratio | compress | decompress |
|---|---|---|---|
| xz -9 (library ratio max) | 56.8% | 3.5 MiB/s | 81 MiB/s |
| brotli -11 | 57.0% | 0.7 | 211 |
| **omnic `best_ratio`** → xz 6 | **57.0%** | 3.9 | 79 |
| Oodle Kraken-5* | ~57.8% | — | ~590 |
| zstd -19 (local Kraken-5 proxy) | 57.9% | 5.4 | 579 |
| **omnic `balanced`** → zstd 9 | 59.5% | 84 | 652 / **1060†** |
| lz4 -12 (speed anchor) | 66.0% | 23 | 718 |

- `balanced` decompresses at **1060 MiB/s — 1.8× Kraken-5's estimated 590** — paying 1.7 pp of ratio for it.
- `best_ratio` beats Kraken-5's ratio by **0.8 pp** at 1/7th its decompress speed.
- zstd-19, the codec the classifier treats as Kraken-5's local equivalent, matches it within measurement noise (0.15 pp / 2% on Silesia).
- No single configuration beats Kraken on both axes at once — no codec in the library occupies that design point. The profile system is how you pick a side.

**Per-file** — omnic's default semantics: each file decided individually, output stays randomly accessible:

| configuration | whole-tree ratio | compressible subset | compress | decompress |
|---|---|---|---|---|
| omnic `balanced` | 68.3% | 31.9% | 9.6 MiB/s | 21.5 MiB/s |
| omnic `best_ratio` | 66.4% | 27.8% | 1.0 | 24.1 |
| zstd -19 on everything | 64.8% | 28.6% | 0.8 | 27.5 |

Per-file costs ~7 pp of ratio (15 KB average files lose cross-file redundancy vs the single stream) and decompress throughput (per-file context setup, ~0.7 ms × 8,586). What it buys: the decision layer routes the **54% already-compressed assets straight to storage** — zero CPU, byte-identical, no double-compression — and any file can be pulled out without touching the rest. Per-file is the right mode when files are assets you load individually; pack-then-compress when they are not.

\* Kraken has no public binary. Its figures are converted estimates: third-party same-machine Silesia measurements of Kraken 2-6, cross-calibrated through a geometric-mean machine factor from five zstd levels; ±8% sensitivity. zstd-19 independently reproduces the Kraken-5 anchor on Silesia, which is why it serves as the proxy row above.
† 1060 = omnic's streaming product path (frame-to-frame dictionary reuse); 652 = one-shot library call on the same data.

## License

MIT. Vendored codecs keep their own licenses, see `THIRD_PARTY_NOTICES.md`. No GPL/LGPL code is linked by default (`OMNIC_ENABLE_GPL` is reserved for optional builds).

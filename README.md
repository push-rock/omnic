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

## The pitch: every file gets the codec it deserves

omnic reads each file once (~1 ms: three 256 KiB samples, extension routing, 8 features) and routes it to the right codec out of 16. Inside one directory that means different files get different answers:

| in a mixed workspace (72 files, 126 MB) | omnic picks | and gets | zstd -19 would get |
|---|---|---|---|
| 22 text files, 35.5 MB | xz 9 | **27.3%** | 28.2% |
| SQLite database, 24.1 MB | xz 6 | **20.6%** | 22.9% |
| flat synthetic bitmap, 24 MB | brotli 9 | **24 MB → 928 B** | 24 MB → 3.0 KB |
| photo bitmap, 26.4 MB | xz 6 | **13.8%** | 15.9% |
| 35 already-compressed PNG/Ogg | **pass-through** | untouched, zero CPU | re-compressed for ~0.01% of the tree |

Same tree, same file-by-file protocol, one command each:

| per-file over the whole tree | ratio | compress | decompress |
|---|---|---|---|
| `gzip -9` | 26.4% | 38 MiB/s | 347 MiB/s |
| `zstd -3` (zstd CLI default) | 26.5% | 261 | 658 |
| `zstd -19` (max squeeze) | 20.4% | 3.2 | 623 |
| **`omnic c`** · balanced | 24.3% | 85 | **736** |
| **`omnic c`** · best_ratio | **19.2%** | 3.0 | 188 |

- "Max compression" is not a single algorithm. `best_ratio` beats `zstd -19` by **1.2 pp** — because per-file routing applies the right compressor everywhere instead of one compromise everywhere.
- `balanced` beats the `zstd` CLI's own default by **2.1 pp of ratio while decompressing faster**, and beats `gzip -9` on all three axes at once.
- If you first tar the tree into one stream, cross-file redundancy is worth several more pp — the [game-asset-tree section](#real-world-a-game-asset-tree) shows that protocol with a 152.5 MB real game tree.

And one thing no single-codec CLI can give you at all: omnic **decodes 16 formats** — gzip, zlib, raw deflate, zstd, xz, lzma, lzip, bzip2, lz4, brotli, snappy, compress (.Z), lzfse, lzvn, plus both lzma container variants. Someone hands you a `.lzfse` or a 1989 `.Z` file, omnic opens it.

(Test corpus: 16 source files, 3 docs, 8 novels + enwik8, 2 bitmaps, 3 fonts, 1 SQLite DB, 3 DLLs, 35 PNG/Ogg — all real files. Reproduce with `omnic_gamepack`.)

## What it does

lzbench benchmarks 50+ compressors, but no mainstream tool routes between them. `zstd --adapt` adapts levels *within* one codec; 7-Zip doesn't care whether the input is JSON or a bitmap. omnic closes that gap:

- **Compress**: classify the input (extension routing + 3-segment sample: 256 KiB head/middle/tail, 8 features), then look up a policy table — *7 content classes × 4 preference profiles* — to pick `(codec, level)`. A full decision takes about a millisecond, most of it I/O.
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
omnic d   [options] <file...>    decompress (auto format by magic/ext; .omnic → whole tree)
omnic pack   [options] <dir>     pack a directory tree into one .omnic archive
omnic unpack [options] <arc>    extract / list / verify a .omnic archive
omnic analyze <file...>          show type/entropy/decision, no side effects
omnic list                       list codecs and recognized formats

-p --profile <v>   fast_compress | fast_decompress | best_ratio | balanced
-c --codec <name>  force a codec            -l --level <n>  codec-native level
-T --threads <n>   worker threads (zstd)    -o --output-dir <dir>
-f --force    overwrite output / compress anyway / --file target exists
-k --keep     keep the source file (default: removed after success)
-x --explain  print the decision rationale  -q --quiet

pack:   --per-file  one block per file (ms single-file extract, worse ratio)
        --no-skip   compress already-compressed assets too (store by default)
        --block-size <MiB>  solid block limit, 1..1024 (default 64)
unpack: --file <path>  extract only this entry (repeatable)
        --list         name/size/codec/ratio without touching the data area
        --verify       check both CRC layers, write nothing
        -C --dir <dir> output root
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

## .omnic archives: the whole tree, decided file by file

`omnic pack` turns the trade-off above into a dial. Every file is classified, but instead of writing 8,586 tiny outputs, same-codec files are concatenated into buckets and compressed as 64 MiB solid blocks. You keep per-file routing *and* cross-file redundancy, in one file:

| strategy | ratio | pack | single file out | when |
|---|---|---|---|---|
| solid (default) | **65.8%** | **4.9 s** | 81 ms | backing up a tree |
| `--per-file` | 68.4% | 8.4 s | **10 ms** | assets you load individually |
| `--no-skip` | 62.2% | 18.9 s | 81 ms | squeezing every last byte |
| best_ratio + solid | **63.2%** | 23.9 s | 81 ms | cold storage |

(Same 152.5 MB game tree; `zip` deflate on it: 69.2%, 3.8 s. `tar -cf - | xz -9`: 56.8%, but single-stream — you cannot pull one file out without decompressing everything.)

What the block layout buys that no plain compressor has:

- **Corruption can't spread.** Each block carries its own CRC-32; a flipped byte kills one block and extraction reports every file it contained, then keeps going: `extracted 7435/8586 files`, exit code 2. A zip with a damaged central directory usually gives you nothing.
- **`--verify`** fully decompresses and checks both CRC layers without writing a single file — the "is this backup good?" question answered in one command.
- **`--list`** reads only the compressed index at the tail: 8,586 entries in 96 ms.
- **`--file`** decompresses only the blocks the target lives in; several targets in one block are merged into a single pass.
- `omnic d` auto-detects the archive magic, so "decompress" just works on `.omnic` like everything else.

The two profiles keep their meaning: `pack` defaults to `balanced` (the 4.9 s / 65.8% row), `-p best_ratio` switches buckets to xz (the 63.2% row). The format, block table and index layout are frozen as of v1 — published archives stay readable forever; future versions may add fields but never rearrange existing ones.

Decompressing the balanced archive takes ~4.4 s on the same machine, against ~2.5 s for `tar -x` on the equivalent zip. The engine itself accounts for 0.3 s of that (block reads, decompression, both CRC layers over all 152 MB — measured via `--verify`); the rest is Windows file-creation cost that a plain directory copy of the same tree pays too (robocopy, single-threaded: 3.7 s). On the axes where archives compete — ratio, pack speed, selective access, fault tolerance — `.omnic` is ahead; on raw extract throughput under a real-time antivirus it trades ~1.8 s on 8,586 files, which v0.3.1's parallel write path is reserved to close.

## License

MIT. Vendored codecs keep their own licenses, see `THIRD_PARTY_NOTICES.md`. No GPL/LGPL code is linked by default (`OMNIC_ENABLE_GPL` is reserved for optional builds).

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

- **Compress**: classify the input (extension + 4 MB entropy sample), then look up a policy table — *7 data classes × 4 preference profiles* — to pick `(codec, level)`.
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

Speed vs ratio for the compressors, measured through omnic's streaming adapter layer on the [Silesia corpus](https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia) (212 MiB, 12 real-world files; median of 3 rounds). Reproduce with the bundled tool:

```
omnic_bench --rounds 3 --csv bench.csv --corpus silesia/dickens --corpus silesia/mozilla ... (12 files)
python tools/plot_bench.py bench.csv -o assets/bench
```

`omnic_bench` also measures adapter overhead against direct library calls on the same data: on Silesia, every codec stays within **0.8% on compression and 1.4% on decompression** (reference: i7-12700, GCC 16.2 MinGW-w64; raw numbers in `assets/bench.csv`). Most decompression paths are actually *faster* through omnic (up to +60%: zstd low levels, lz4, snappy): fixed-size chunk buffers stay cache-hot, while one-shot baselines stream through buffers larger than L3. That margin is not free — it comes from seek-based size preallocation for whole-buffer codecs, 1 MiB stdio buffering for small-chunk formats, hardware CRC32C (SSE4.2/ARM64) in the snappy framing, and a block-header pre-scan that sizes lzfse output exactly. `ctest` runs a 40% adapter-overhead smoke gate (`bench_smoke`, synthetic corpus — self-contained and reproducible in CI) on every push; the tight thresholds are enforced by local runs of the same tool.

## How it picks

The policy table is compiled in (`src/policy/policy_table.gen.c`), initialized from lzbench measurements; a data-driven retraining pipeline is planned for phase 2. Deciding is a table lookup — microseconds, no model runtime.

## License

MIT. Vendored codecs keep their own licenses, see `THIRD_PARTY_NOTICES.md`. No GPL/LGPL code is linked by default (`OMNIC_ENABLE_GPL` is reserved for optional builds).

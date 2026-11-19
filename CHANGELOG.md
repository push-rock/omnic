# Changelog

All notable changes to omnic. Format follows [Keep a Changelog](https://keepachangelog.com); versioning is semantic.

## [0.3.3] — 2026-11-19

### Changed — parallel block compression in pack (T12)

- `pack` now compresses blocks through a worker pool: the main thread
  builds blocks (file reads, CRC) and dispatches, workers compress, and
  receipts are consumed in dispatch order — so the block table, CRCs
  and file layout stay identical to the serial path. `-T` follows the
  classification-pool semantics (0 = 8 workers by default, 1 = serial,
  n = n workers); `-T 1` and parallel archives are byte-identical by
  construction, and intra-block codec MT is disabled in pack (workers
  compress single-threaded). Stream mode (`omnic c -T`) keeps
  libzstd/liblzma MT unchanged.
- The best-ratio bucket probe (which pits the planned codec against
  xz/9 for buckets ≥ 4 MiB) now decides on a 4 MiB prefix sample
  instead of compressing the whole first block twice — serial packs
  save the duplicate full-block pass, and in the pool the probe result
  is peeked (not queued behind older receipts) so the bucket's codec is
  settled while earlier blocks are still in flight.
- Measured on the 152.6 MB game tree (8,588 files, best of 3): pack
  `best_ratio` 24.2 s → **11.5 s**, `--per-file` 5.8 s → **2.5 s**,
  `--no-skip` 16.2 s → 12.7 s, default 2.1 s → 1.9 s (zstd blocks were
  already fast; the old path let libzstd MT run inside a block, the
  pool trades that for block-level parallelism — parity on this tree,
  recorded as such). Silesia (202 MiB, 12 files): default 2.41 s →
  **1.06 s**, `best_ratio` 87.4 s → **30.2 s** — at the max tier omnic
  now packs this shape 1.7× faster than `7z -mx=9` (50.8 s). All
  archive sizes are byte-for-byte unchanged from 0.3.2.
- New gate `test_pack_parallel_blocks`: an 18 MiB tree shaped to force
  every pool hazard — 16 pool sequences (14 blocks + probe + wrapped
  font block) against a 9-slot receipt ring, the best-ratio probe
  landing past the wrap point, a store bucket between compressed
  buckets, and window backpressure. Packs at `-T 1` vs parallel under
  both `balanced` and `best_ratio` must be byte-identical, plus a full
  roundtrip of the parallel archive. Two real bugs were caught by
  exactly this shape during development (an aliased receipt slot read
  as success, and a metadata-ring overwrite when the probe submit
  skipped backpressure) — the gate fails on both when reintroduced.

### Changed — per-file codec path: context reuse and right-sized xz dictionaries (A1/A2)

- The zstd adapter keeps its encoder/decoder contexts and 2×4 MiB chunk
  buffers in thread-local storage, reset at entry (`ZSTD_*Ctx_reset`,
  the supported reuse path) — a stream aborted mid-frame would
  otherwise poison the next one. Per-file packs call the codec once
  per file, so rebuilding contexts and buffers per call was a fixed
  tax paid thousands of times. brotli was audited for the same
  treatment and skipped: the vendored version exposes no instance-reset
  API, and brotli occupies four decision cells, none on the per-file
  hot path.
- The xz encoder sizes its LZMA dictionary to the input when the whole
  input is known and under 8 MiB (power-of-two ceiling, capped at the
  preset default): match distances cannot exceed the input length, so
  nothing is lost — the game-tree best-ratio per-file archive actually
  came out 133 bytes smaller. Inputs ≥ 8 MiB or of unknown size keep
  the full preset dictionary: solid blocks, stream mode and the
  Silesia/dickens regression invariants are byte-identical. This was
  the real engine behind the slow per-file best-ratio story — each
  small block paid a full preset-dictionary setup, not decoder-context
  churn as the old roadmap note assumed.
- Measured on the 152.6 MB game tree, same window, best of 3:
  `--per-file` pack 2.50 s → **1.75 s** (-30%), `--per-file -p
  best_ratio` 20.8 s → **6.0 s** (-71%); the balanced per-file product
  is byte-identical to 0.3.2 (SHA-256-verified).
- Gates: `test_ctx_reuse` (three clean zstd roundtrips in a row, then a
  truncated stream followed by a good one — removing the reset fails
  it, the exact 10-red shape caught during development) and
  `test_xz_small_dict` (parses the xz block header's LZMA2 filter
  props — a format-spec field, stable across liblzma versions — and
  asserts a 100 KB input declares a 128 KiB dictionary; reverting to
  full preset dictionaries fails it; boundary sizes 1 B … 1 MiB+1
  roundtrip).

### Changed — parallel block decode for unpack/verify (B)

- `unpack` and `--verify` decode blocks through a worker pool using
  the same dispatch-order receipt pattern as pack: workers read the
  archive through independent handles and decompress, the main thread
  reaps results in dispatch order — block order, bad-block isolation
  and per-entry CRC semantics are identical to the serial path.
  `--file` extraction stays serial (latency-sensitive).
- The pool is gated on block granularity: `--verify` has no write
  competition and always benefits, while during unpack with the write
  pool active it only engages for small blocks — concurrent 64 MiB
  solid decodes plus 8 writers contend for memory bandwidth (measured
  +21% on solid unpack; serial decode + parallel writes is already the
  better pipeline there).
- Measured on the game tree, same window, best of 3: `--verify`
  `--per-file` 0.73 s → **0.43 s** (-41%), `--per-file -p best_ratio`
  1.04 s → **0.46 s** (-56%), solid 0.55 s → **0.42 s** (-24%). Unpack
  wall clock is unchanged — write-bound on this machine (Defender's
  per-file tax), with the decode saving sitting inside that noise.
- Gate `test_decode_pool`: a 66-block per-file archive must unpack to
  identical trees at `-T 8` and `-T 1`, verify counts must match
  across thread counts, and a corrupted mid-archive block must kill
  exactly its own member while both neighbors survive.
  Mutation-checked: dropping the receipt sequence validation
  segfaults on the stale-slot path instead of returning wrong data.

### Added — multithreaded regression coverage

- `test_core` compresses a 12 MB corpus with zstd and xz at `-T 2/8` and
  byte-compares the decompressed result: the corpus compresses to ~9.6 MB,
  beyond the single output buffer where v0.3.1's zstd truncation hid.
  Mutation-validated — restoring the old single-finish loop fails the checks.
- `test_archive` packs the 121-file tree at `-T 1` and `-T 8` and asserts
  byte-identical archives, locking the classification-determinism promise.
- CI gained a Linux/clang ThreadSanitizer job instrumenting the whole tree
  (worker pools and multithreaded codecs included), running the core and
  archive suites. Two real-time budget tests (trial compress, decision
  latency) skip under TSan — their 50 ms budgets assume native speed and
  the instrumented entropy scan runs ~40x slower; all correctness checks
  still run.

## [0.3.2] — 2026-11-17

### Fixed — multithreaded zstd truncation (v0.3.1, data integrity)

- `c`/`pack` with `-T` > 1 on zstd no longer silently truncate output. The
  streaming loop exited after a single finish call; single-threaded zstd
  drains its small internal buffer across that call, but the multithreaded
  job pool holds all output until the end — the first finish filled exactly
  one output chunk and the loop stopped, producing a cut-off stream
  (verify/extract then flag the block as corrupted; nothing passes silently,
  but the packed data is unusable). The finish call now loops until the
  encoder reports completion. v0.3.1 archives made with zstd and `-T` > 1
  should be re-packed. Audited the other streaming codecs: xz/lzma loop until
  STREAM_END, brotli until `IsFinished` — zstd was the only one-shot finalizer.

### Added — parallel pack classification

- `pack` classifies the tree through a worker pool: 8 workers by default,
  `-T <n>` to tune, `-T 1` for the serial path. Classification (per-file
  open plus 3-segment sampling — filesystem/antivirus latency, not CPU) was
  the wall-clock majority of packing: 3.63 s of 5.03 s on the 152.6 MB,
  8,586-file game tree. The pool cuts it to 0.73 s: pack 2.4x faster overall
  (5.03 -> 2.12 s), 2.5x faster than .NET zip (5.23 s) on the same tree in
  the same window.
- Determinism preserved: parallel and serial runs produce byte-identical
  archives — plans are per-file, bucketing and block writes stay on the main
  thread. `-x/--explain` and trees under 32 files keep the serial path.
- `-T` now sets workers for classification, xz/zstd compression and unpack
  writes.

### Added — multithreaded xz

- `-T` > 1 now multithreads xz encoding through liblzma's stream MT encoder
  (block size follows the xz CLI convention at 3x dictionary; memory capped at
  2 GiB with automatic thread reduction). Default stays single-threaded and
  byte-identical. Silesia 202 MB tarball at best_ratio, single stream:
  63.0 s -> 11.6 s at `-T 8`, ratio 23.22% -> 23.36% (+0.14 pp from
  independent block splitting).

### Fixed — classification trusts content over extensions

- Content magic bytes now outrank file extensions in the routing decision.
  A renamed PNG/ZIP/JPEG is passed through untouched instead of being
  pointlessly recompressed; text renamed to `.jpg` is compressed instead of
  being stored raw (previously the extension's claim was final).
- Entropy guardrail both ways: an extension claiming "already compressed" is
  revoked when sampled entropy is clearly compressible (< 7.5 bits/byte);
  text/structured claims are revoked at ≥ 7.5, so random data named `.txt`
  is skipped instead of white-compressed.
- PDF content is deliberately not asserted by magic: PDFs mix raw and Flate
  streams (Silesia's `reymont` is one — entropy 4.79, compresses 3×), so the
  `already compressed` verdict for PDFs stays extension-only.
- Regression: Silesia 12-file pack unchanged to the byte (28.97%,
  61,402,164 B); 457 + 105 suite checks green; 5/5 fixture roundtrip
  SHA-identical.

### Added — `--file` patterns

- `unpack --file` now takes patterns in three forms: an exact entry path
  (unchanged), a directory prefix (`--file src/` extracts the whole subtree),
  or a glob (`--file "*.lua"`). Globs match the full stored path, `*` crosses
  `/`, matching is case-sensitive. A pattern that matches nothing is still a
  usage error — all-or-nothing semantics are unchanged, and single-pattern
  single-file extraction keeps its 8.7 ms headline path.

### Fixed — CLI ergonomics

- `pack` now rejects `-c/--codec` and `-l/--level` with a usage error instead
  of silently ignoring them — pack routing is per-file by design; use
  `omnic c` for a single forced stream.
- `-c` now carries compression intent on its own: compressing an
  already-compressed or random file with an explicit codec no longer also
  requires `-f` (previously the skip policy fired first and the manual codec
  was silently unreachable).
- `analyze` now honors its options. The CLI path previously called the
  options-less library entry point, so `-c` and `-p` were silently dropped:
  `analyze -p best_ratio` reported the balanced verdict, and `analyze -c` could
  say "skip" while `c -c` would actually compress. Both now report what
  `c`/`pack` would really do.
- Skip messages on `c` print the `use --force` hint unconditionally instead of
  hiding it behind `-x`.

## [0.3.1] — 2026-11-10

### Added — parallel unpack writes

- `omnic unpack` extracts through a worker pool (8 writers by default, `-T`
  to tune, `-T 1` forces the serial path): the main thread decodes blocks and
  runs the per-entry CRC checks, workers do the disk writes. Bad-block
  isolation, `--file` filtering, reporting and exit codes are unchanged.
- Unpack of the 152.5 MB game tree (8,586 files): 4.3 s serial → 3.9 s on
  Windows with Defender real-time scanning active (3.6 s at `-T 2`); most of
  the residual gap is the antivirus scanning each new file from an unsigned
  process. Without real-time AV (or with the output directory excluded) the
  writers deliver their full speedup.
- Unpacks with fewer than 32 write jobs keep the serial path — single-file
  `--file` extraction stays at its 8.7 ms headline speed.
- Index-driven block bucketing replaces the per-block table scan; the serial
  path is ~0.15 s faster on per-file archives.
- `-T` now sets worker threads for both zstd packing and unpack writes.

### Fixed

- Parallel unpack: a slot buffer could be freed while the main thread was
  still dispatching entries from it — workers that drained the queue faster
  than the producer left it dangling, corrupting entry CRC checks. Surfaced
  on Linux/macOS CI (2–4 cores, 9 threads), never on the many-core Windows
  dev box. Slot lifetime is now gated on an explicit close from the producer.
- `--file` extraction regressed to writing every entry of a target block
  instead of only the requested names; the filter is restored and pinned by
  a test asserting non-target files stay absent.
- `omnic analyze` could print an empty codec / level 0 on GCC 16.2 `-O2`/`-O3`
  builds: the optimizer wrongly eliminates the stack initialization of the
  all-zero default-options literal, so the decision ran on a garbage profile.
  The fallback is now a `static const` object in `.bss`, which the compiler
  cannot elide. `test_core` gained content assertions (codec/level/reason
  non-empty) so the class of failure is caught wherever it compiles.
- Decompression reports now state throughput in output MB/s (restored bytes per
  second), matching zstd/lzbench/Oodle convention; it previously reported
  compressed-bytes/s, understating decode speed by the compression ratio.

## [0.3.0] — 2026-11-05

### Added — `.omnic` archives

- `omnic pack <dir>` packs a whole tree into one file: every file is classified
  by the same decision engine as `omnic c`, grouped into per-codec buckets, and
  compressed as solid blocks (64 MiB limit, `--block-size` to change, 1..1024).
  Files the classifier would skip (already-compressed assets) are stored
  as-is by default; `--no-skip` compresses them too.
- `omnic unpack <arc>` extracts the tree; `omnic d` auto-detects the archive
  magic and does the same thing.
- `--per-file` packs one block per file: single-file extraction drops to
  milliseconds at ~2.7 pp worse ratio.
- `--file <path>` (repeatable) extracts only the named entries; blocks that
  hold none of them are never decompressed, and several targets in one block
  share a single pass.
- `--list` prints name, size, block codec and block ratio straight from the
  compressed tail index — the data area is never touched (8,586 entries in 96 ms).
- `--verify` fully decompresses and checks both CRC layers, writing nothing.
- Windows paths round-trip through UTF-16 internally; entry names are UTF-8
  on disk. Symlinks, mode bits and ACLs are skipped (v1 scope).

### Robustness

- Every block carries a CRC-32 over its compressed bytes, every entry one over
  its raw bytes. A corrupted block is skipped and reported with the affected
  file names; extraction continues and exits with code 2. A damaged index or
  footer is refused outright.
- Guards against malformed archives: header/footer cross-check, duplicate
  entry names, `../` path escape, oversized `name_len`, block/offset bounds —
  all rejected with an actionable message, never a crash (fuzz-checked).
- `pack` refuses to write the archive inside the source tree.

### Performance

- Unpack writes only through `omnic_fs_fopen` (UTF-8 safe on Windows), creates
  parent directories only on demand, and sets mtimes on the open handle.
- Measured on the 152.5 MB game tree (i7-12700, Windows): solid+balanced pack
  4.9 s / 65.8%; per-file 8.4 s / 68.4%; no-skip 18.9 s / 62.2%;
  best_ratio+solid 23.9 s / 63.2%; `--verify` 0.6 s. zip (bsdtar deflate)
  on the same tree: 3.8 s / 69.2%.

### Fixed

- `omnic c`/`d`/`analyze` on non-ASCII (GBK) Windows paths now open files
  through the UTF-8 layer instead of the CRT's ANSI `fopen`.
- Decision pass on directories no longer assumes the balanced profile.
- 0-byte files, empty trees and empty directories round-trip correctly.

## [0.2.1] — 2026-11-04

- README rewrite with quantified tables; man page.

## [0.2.0] — 2026-11-03

- 16 codecs, policy table with four profiles, entropy/extension classification,
  format auto-detection by magic bytes, per-codec roundtrip suite (524 checks).

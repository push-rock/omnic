# Changelog

All notable changes to omnic. Format follows [Keep a Changelog](https://keepachangelog.com); versioning is semantic.

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

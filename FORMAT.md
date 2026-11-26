# The .omnic Archive Format — Specification (v1)

> Status: **frozen**. Any archive written as v1 is readable by every future
> version of omnic. This document is the complete, byte-level definition:
> enough to write an independent decoder.

## Why a container spec

Most archive formats weld themselves to one algorithm — and inherit its
ceiling. `.omnic` takes the opposite bet: the **container is a thin, stable
contract** (block layout, index, two CRC layers), while **codecs are
replaceable parts**. Every block records which codec produced it; the
unpacker dispatches on that. When zstd gets faster next year, every existing
`.omnic` archive gains the improvement for free on decode — and the archive
you wrote today is already laid out to accept codecs that did not exist when
it was written.

The contract fits on one page. That is the point.

## Layout

```
+---------+-----------------+------------------+-----------------+---------+
| Header  |     Data        |    BlockTable    |    EntryTable   | Footer  |
|  32 B   |  blocks 0..N-1  |  (merged into an Index region,   |  32 B   |
|         |                 |   one zstd frame, level 3)        |         |
+---------+-----------------+------------------+-----------------+---------+
```

All multi-byte integers are **little-endian**. Block-table offsets are
relative to `data_off`; footer offsets are absolute file offsets.

## Header (32 bytes)

| off | size | field    | value |
|----:|-----:|----------|-------|
| 0   | 8    | magic    | ASCII `OMNICARC` |
| 8   | 2    | version  | 1 |
| 10  | 2    | flags    | bit0 `HAS_MTIME`; bit2 `INDEX_ZSTD` (always 1 in v1); others 0 |
| 12  | 4    | n_blocks | ≥ 0 (0 = empty archive) |
| 16  | 4    | n_entries| ≥ 0 |
| 20  | 4    | data_off | 32 (fixed in v1; reserved for future header growth) |
| 24  | 8    | reserved | 0 (readers ignore non-zero) |

## Data region and blocks

Blocks are packed back-to-back with **no separators and no per-block
headers** — a compressed block is a single standard codec stream (xz, zstd,
brotli, …) that carries its own magic; all physical attributes live in the
block table.

- **Store block** (`fmt = 0`): the raw bytes of its member files,
  concatenated verbatim.
- **Compressed block**: the concatenated raw bytes of its member files,
  compressed once into a single stream of the codec named by `fmt`.
- A file never straddles blocks. Blocks accumulate whole files up to the
  block-size cap (default 64 MiB, `--block-size` accepts 1..1024); a single
  file larger than the cap becomes its own oversized block, never split.

## Index region

The index plaintext is `BlockTable ‖ EntryTable`, compressed as **one zstd
frame at level 3** and written after the Data region. The footer records its
offset, compressed length, and CRC.

**BlockTable** — 32 bytes per block, in block-id order:

| off | size | field     | meaning |
|----:|-----:|-----------|---------|
| 0   | 1    | fmt       | codec id (table below); store block = 0 |
| 1   | 1    | level     | audit only; unpackers ignore |
| 2   | 2    | pad       | 0 |
| 4   | 8    | offset    | block start, relative to `data_off` |
| 12  | 8    | comp_size | bytes in the archive (store block: = raw_size) |
| 20  | 8    | raw_size  | bytes after decompression (store block: = comp_size) |
| 28  | 4    | crc32     | CRC-32 of the **stored** block bytes (compressed or verbatim) |

**EntryTable** — variable-length records, in pack traversal order:

| field        | size | meaning |
|--------------|-----:|---------|
| name_len     | 2    | UTF-8 byte length, 1..65535 |
| name         | …    | path, rules below |
| type         | 1    | 0 = file, 1 = directory |
| pad          | 1    | 0 |
| block_id     | 4    | owning block; directory entries = 0xFFFFFFFF |
| in_block_off | 8    | file start inside its block (directory: 0) |
| orig_size    | 8    | original byte count (directory: 0) |
| orig_crc32   | 4    | CRC-32 of the **original** file bytes (directory: 0) |
| mtime        | 8    | Unix seconds; present for all entries iff header flag bit0 |

The mtime flag is global: either every record carries mtime or none does.

## Footer (32 bytes)

| off | size | field           | meaning |
|----:|-----:|-----------------|---------|
| 0   | 8    | magic           | ASCII `OMNICEND` |
| 8   | 8    | index_off       | absolute; ≥ data_off |
| 16  | 8    | index_comp_size | > 0 |
| 24  | 4    | index_crc32     | CRC-32 of the compressed index bytes |
| 28  | 2    | entries         | cross-checked against header n_entries |
| 30  | 2    | blocks          | cross-checked against header n_blocks |

## Codec ids (`fmt`)

| id | codec | stream |
|---:|-------|--------|
| 0  | store | raw bytes |
| 1  | gzip | RFC 1952 |
| 2  | zlib | RFC 1950 |
| 3  | deflate (raw) | RFC 1951 |
| 4  | zstd | standard frame |
| 5  | xz | standard container |
| 6  | lzma (alone) | legacy header |
| 7  | lzip | standard container |
| 8  | bzip2 | standard stream |
| 9  | lz4 | standard frame |
| 10 | brotli | standard stream |
| 11 | snappy | standard stream |
| 12 | compress (.Z) | LZW legacy |
| 13 | lzfse | Apple stream |
| 14 | lzvn | Apple raw |
| 15 | lzham | — |
| 16 | lzav | — |

Unknown ids: skip the block with a corruption report (see below) — never
crash, never mis-decode.

## CRC

CRC-32/ISO-HDLC (the zip polynomial), init 0, no final XOR — identical to
zlib's `crc32()`. Used in three places: per-block (stored bytes), per-entry
(original bytes), and the index region (compressed bytes). The two data-layer
CRCs are what buy corruption *isolation*.

## Path rules

UTF-8, `/` separators, relative to the archive root. An entry name must not
contain: a leading `/`, a drive letter (`C:`), any `..` element, backslashes,
NUL, or control characters (< 0x20). Names must be unique — a duplicate is
corruption, and the unpacker refuses it.

## Packing strategies

| strategy | mapping | trades |
|----------|---------|--------|
| solid (default) | classify → bucket → cut at 64 MiB file boundaries | best ratio |
| `--per-file` | one block per file | millisecond single-file extraction, ~3 pp ratio cost |
| `--no-skip` (orthogonal) | already-compressed files join the compression buckets | a little more ratio, more CPU |

## Unpacker behavior

Open sequence: read the last 32 bytes, verify `OMNICEND`, cross-check
counts; seek to `index_off`, verify the index CRC, decompress with zstd;
parse and validate both tables (path rules, bounds, uniqueness). Any failure
is fatal for the archive — a damaged index means no listing and no
extraction (v1 keeps no index mirror).

**Corruption isolation** — the format's headline property:

- Block CRC mismatch or decode failure → *that block's* entries are skipped
  and reported by name; every other block extracts normally. One flipped bit
  kills one block, not the archive.
- Entry CRC mismatch (block stream intact, content wrong) → that file is
  skipped and reported; extraction continues.
- Final summary reports `extracted N/M files`; exit code 2 (`E_DATA`).

Exit codes: 0 OK, 1 usage, 2 data (corruption, path escape, CRC), 3 system
(I/O, memory). Decompression of a block whose `raw_size` exceeds 1 GiB is
refused — the zip-bomb guard.

## Limits (v1, honest)

- ≤ 65,535 blocks and ≤ 65,535 entries per archive; exceeding this is
  rejected at pack time.
- Memory: pack ≈ max(block size, index plaintext); unpack ≈ same.
- No symlinks, POSIX modes/ACLs, hard-link dedup, or `--exclude` in v1.
- The reference implementation's path buffer is 4096 bytes; longer paths are
  reported per-file rather than packed.

## Compatibility promise

v1 is frozen: published fields never reorder, published semantics never
change. Evolution uses only reserved space — header/footer `reserved` bytes,
unused flag bits, and new `type` values (e.g. 2 = symlink in some future
version). The index codec is fixed at zstd so that a v1 decoder only ever
needs one index decompressor. The reference implementation lives in this
repository (`src/core/archive.c`); the container is deliberately small
enough to reimplement independently in an afternoon.

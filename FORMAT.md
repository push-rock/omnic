# The .omnic Archive Format — Specification (v2)

> Status: **v1 frozen, v2 current.** Every archive ever written as v1 stays readable by
> every future version of omnic. v2 adds what v1 left out — permission bits and Windows
> file attributes, symlinks and junctions, hard links, creation times, and 32-bit entry
> counts — and is what omnic writes now. This document is the complete, byte-level
> definition: enough to write an independent decoder for either version.

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
| 8   | 2    | version  | 1 or 2 (readers refuse anything else) |
| 10  | 2    | flags    | bit0 `HAS_MTIME`; bit2 `INDEX_ZSTD` (always 1). v1: bit0 says whether records carry mtime. v2: records always carry it, so the flags must be exactly `0x0005`. Any other bit → the archive is refused |
| 12  | 4    | n_blocks | ≥ 0 (0 = empty archive) |
| 16  | 4    | n_entries| ≥ 0 |
| 20  | 4    | data_off | 32 (readers refuse any other value) |
| 24  | 4    | ext_off  | v2: file offset of the extension area; 0 = there is none. v1: 0 |
| 28  | 4    | ext_crc32| v2: CRC-32 of the extension area's bytes; 0 when `ext_off` is 0 (one without the other is refused) |

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

**EntryTable** — variable-length records, in pack traversal order. A record
is `name_len`, `name`, a fixed part, and the optional fields its flags ask
for, in this fixed order.

| field        | size | meaning |
|--------------|-----:|---------|
| name_len     | 2    | UTF-8 byte length, 1..65535 |
| name         | …    | path, rules below |
| type         | 1    | 0 = file, 1 = directory, 2 = symbolic link, 3 = hard link |
| flags        | 1    | see below |
| block_id     | 4    | owning block; directory and hard-link entries = `0xFFFFFFFF` |
| in_block_off | 8    | start inside its block (directory, hard link: 0) |
| orig_size    | 8    | byte count of the entry's data (directory, hard link: 0) |
| orig_crc32   | 4    | CRC-32 of the entry's **original** bytes (directory, hard link: 0) |
| mtime        | 8    | Unix seconds; always present in v2 (0 = unknown) |
| link_to      | 4    | hard link only (`HAS_LINKTO`): index of the file entry that holds the data; it must be a type-0 entry, not the entry itself |
| ctime        | 8    | `HAS_CTIME`: creation time, Unix seconds |
| mode         | 2    | `HAS_MODE`: POSIX permission bits, `07777` (setuid/setgid/sticky included) |
| attrs        | 2    | `HAS_ATTRS`: Windows attributes, only `0x0001` read-only, `0x0002` hidden, `0x0004` system |

| flag | bit | meaning |
|------|----:|---------|
| `LINK_DIR` | 0x01 | the link target is a directory (type 2) |
| `LINK_JUNCTION` | 0x02 | a Windows junction: an absolute target (type 2) |
| `HAS_CTIME` | 0x04 | `ctime` follows |
| `HAS_MODE` | 0x08 | `mode` follows |
| `HAS_ATTRS` | 0x10 | `attrs` follows |
| `HAS_LINKTO` | 0x40 | `link_to` follows (type 3) |

A record carries only the fields it has, so an ordinary file or directory
whose permissions and times are the defaults is exactly as long as it was in
v1. Unknown flag bits make the archive invalid: a new field means a new
version, never a new bit.

- **Symbolic link (type 2):** the entry's data is the target path, verbatim,
  as UTF-8 — stored in a block like file content, and CRC-checked the same
  way. It may be relative or absolute, and it is *not* required to point
  anywhere inside the archive.
- **Hard link (type 3):** no data. The first directory entry for a file
  holds the data; every further name for the same file points at it. Names
  are assigned in index order, so the archive is deterministic.
- **Directory (type 1):** no data. The record can carry `mode` and `attrs`;
  its `mtime` is kept for listing only (see *Unpacker behavior*).

**v1 records**, for readers that must handle old archives: `name_len`,
`name`, then 26 bytes — `type` (0 or 1), `pad` (0), `block_id`, `in_block_off`,
`orig_size`, `orig_crc32` — plus `mtime` iff header flag bit0 is set. The
reference v1 writer stored `block_id = 0` for directories, not
`0xFFFFFFFF`; readers accept both.

## Footer (32 bytes)

| off | size | field           | meaning |
|----:|-----:|-----------------|---------|
| 0   | 8    | magic           | ASCII `OMNICEND` |
| 8   | 8    | index_off       | absolute; ≥ data_off |
| 16  | 8    | index_comp_size | > 0 |
| 24  | 4    | index_crc32     | CRC-32 of the compressed index bytes |
| 28  | 4    | counts          | v1: `entries` (2) and `blocks` (2), cross-checked against the header. v2: reserved, 0 |

In v2 the 32-bit counts in the header are the only counts, and they must
match the index exactly: the reader refuses an archive whose entry table
runs past the end of the index or leaves bytes unread.

## Extension area (v2)

This is where metadata that not every archive has goes: file ownership and
POSIX extended attributes. It was added without a version change, and that
is the point of its design — an older v2 reader that knows nothing about it
simply never looks at header bytes 24..32, opens the archive, and extracts
every file; it just does not restore owners or xattrs.

- **Layout:** `[Data][Index][Extension area][Footer]`. The area starts
  exactly where the index ends and stops exactly where the footer starts.
  The header's `ext_off` must equal `index_off + index_comp_size`; the
  length is not stored anywhere, it follows from the two positions.
- **Contents (uncompressed, covered by `ext_crc32`):** a list of records,
  one per entry that has extension data, in ascending entry-index order:

  | field       | size | meaning |
  |-------------|-----:|---------|
  | entry_index | 4    | index into the EntryTable |
  | payload_len | 4    | bytes of payload that follow |
  | payload     | …    | tag-length-value records, below |

- **Payload tags:**

  | tag  | value |
  |------|-------|
  | 0x01 | `uid`: 4 bytes, the owning user (never 0) |
  | 0x02 | `gid`: 4 bytes, the owning group (never 0) |
  | 0x10 | xattr blob: 4-byte length, then one or more records of `name_len u16`, `name` (UTF-8), `val_len u32`, `value` |

  Each tag may appear at most once per entry; the xattr records must fill
  their declared length exactly. An unknown tag, a duplicate, an entry index
  out of range or out of order, or a CRC mismatch makes the whole archive
  invalid — it is never "ignored".
- **When it is absent:** an archive packed from a tree with no owners or
  xattrs to keep (any Windows tree, by default) has `ext_off = 0` and is
  byte-for-byte the length it would be without this feature.
- **Restoring:** owners are only restored by a reader running as root; a
  plain user gets the files with their own ownership and a note, which is
  what `tar` does. `ext_off` is 32 bits, so an archive larger than 4 GiB
  cannot carry an extension area; `pack` then leaves it out rather than
  write a truncated offset.

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
- Final summary reports `extracted N/M entries`; exit code 2 (`E_DATA`).

Exit codes: 0 OK, 1 usage, 2 data (corruption, path escape, CRC), 3 system
(I/O, memory). Decompression of a block whose `raw_size` exceeds 1 GiB is
refused — the zip-bomb guard.

**Order of operations** — three rules the format depends on:

1. Directories first, then files, then hard links, then symbolic links,
   then directory permissions. No file is ever written through a link the
   unpacker itself created, so an archive cannot use a link to escape the
   output directory.
2. Permission bits and attributes are applied after the data is written: a
   read-only file must not block its own write, and a read-only directory
   must not block the files that go into it.
3. File mtimes and permissions are restored; directory mtimes are not —
   writing files into a directory updates it, so only an empty directory
   could ever have kept its original time.

A hard link needs its target on disk. With `--file`, when only the hard-link
entry is selected, it is materialised as an independent copy of the data.
On Windows, creating a file symbolic link needs a privilege (or Developer
Mode); without it the entry is reported as failed and the exit code is 2. A
directory symbolic link with an absolute target falls back to a junction,
which needs no privilege, and the summary says so.

## Limits (v2, honest)

- Entry and block counts are 32-bit; the practical ceiling is memory (pack
  keeps the entry list and the index in RAM).
- Not stored: FIFOs, sockets and device files, and reparse points other
  than symlinks and junctions (cloud-file placeholders, dedup, app execution
  aliases). `pack` reports every entry it skips; `--strict` turns a skip
  into a failure.
- Not stored: ACLs, NTFS alternate data streams. Owners and POSIX extended
  attributes are stored in the extension area (see above); xattrs only with
  `pack --xattr`.
- Memory: pack ≈ max(block size, index plaintext); unpack ≈ same.

## Compatibility promise

v1 is frozen: published fields never reorder, published semantics never
change, and every v1 archive stays readable. v2 is the current version and
is held to the same promise from now on. Two rules govern what comes next:

1. **Never add a field to an existing record, and never a new flag bit.**
   A change to the header, the footer, the block table or the entry records
   is a new version number.
2. **New metadata goes into the extension area.** It carries its own
   tags, so it can grow without touching anything a v2 reader already
   parses. An older v2 reader opens such an archive and extracts every
   file; it just does not restore what it does not know. A reader that
   knows the area refuses one it cannot fully parse — unknown tag, bad
   order, bad CRC — rather than guess.

The only bytes a reader ignores are the footer's last four. The index
codec is fixed at zstd so that a decoder only ever needs one index
decompressor. The reference implementation lives in this repository
(`src/core/archive.c`); the container is deliberately small enough to
reimplement independently in an afternoon.

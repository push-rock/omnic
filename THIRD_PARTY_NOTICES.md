# Third-party notices (SYS-CODEC-003)

All codec libraries are vendored under `third_party/` with unmodified sources
(release tarballs; tests/docs pruned where noted). Versions and licenses:

| Directory | Upstream | Version | License |
|---|---|---|---|
| third_party/zstd | https://github.com/facebook/zstd | 1.5.7 | BSD-3-Clause |
| third_party/lz4 | https://github.com/lz4/lz4 | 1.10.0 | BSD-2-Clause |
| third_party/xz | https://github.com/tukaani-project/xz | 5.8.4 | 0BSD |
| third_party/zlib | https://github.com/madler/zlib | 1.3.2 | zlib |
| third_party/zlib-ng | https://github.com/zlib-ng/zlib-ng | 2.3.3 | zlib |
| third_party/libdeflate | https://github.com/ebiggers/libdeflate | 1.26 | MIT |
| third_party/brotli | https://github.com/google/brotli | 1.2.1 | MIT |
| third_party/snappy | https://github.com/google/snappy | 1.3.1 | BSD-3-Clause |
| third_party/lzfse | https://github.com/lzfse/lzfse | master (1.0) | Apple BSD-style |

Notes:
- Source archives are fetched by `tools/fetch_third_party.ps1` (with a mirror
  prefix for restricted networks; direct GitHub URLs are canonical).
- zstd's tarball contains symlinks in `tests/`; on Windows that directory is
  excluded at extraction. `programs/` is not needed for the library.
- No GPL/LGPL library is vendored (admission rule SYS-CODEC-002).
- The omnic project itself is MIT (see LICENSE); each vendored subtree remains
  under its own license, reproduced in the upstream source files.

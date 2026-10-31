#!/usr/bin/env python3
"""omnic decompression fuzz smoke (TST-DEC-001 / SYS-DEC-003).

Per format: take a known-good artifact, mutate (truncations, bit flips,
magic破坏, empty/1-byte/magic-only boundaries), run `omnic d -k`, assert:
  - non-zero exit and no crash  -> PASS (correct error)
  - exit 0 and output == golden -> PASS (tolerant recovery)
  - exit 0 and output != golden -> FAIL silent corruption
  - negative/huge exit code     -> FAIL crash
  - timeout                     -> FAIL hang

Usage: python tools/fuzz_smoke.py [--omnic PATH] [--workdir DIR] [--quick]
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile
import time

OMNIC_DEFAULT = None
SRC_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # tools/ 的上级


def find_default_omnic():
    """单配置生成器在 build/，MSVC 多配置在 build/Release/；相对源根锚定（与 cwd 无关）。"""
    cands = []
    for sub in ("", "Release/"):
        for name in ("omnic.exe", "omnic"):
            p = os.path.join(SRC_ROOT, "build", sub + name)
            if os.path.isfile(p):
                return p
            cands.append(p)
    return cands[0]  # 不存在时报错用（提示安装路径）

# 内嵌合法样本（同 tests/test_core.c：bzip2 blob_a / lzip blob，原文均为 pattern×N）
BZIP2_BLOB = bytes([
    66, 90, 104, 57, 49, 65, 89, 38, 83, 89, 147, 29, 173, 234, 0, 10, 139, 217, 128, 0, 16,
    64, 0, 16, 0, 62, 39, 204, 48, 48, 0, 216, 5, 52, 200, 196, 196, 196, 41, 166, 70, 38, 38,
    32, 82, 170, 6, 70, 158, 154, 155, 83, 97, 22, 194, 47, 34, 47, 194, 47, 2, 44, 136, 178,
    34, 236, 34, 228, 69, 236, 69, 129, 23, 2, 44, 4, 91, 132, 88, 17, 122, 17, 124, 17, 100,
    69, 161, 22, 132, 91, 136, 176, 34, 250, 34, 232, 69, 208, 139, 145, 23, 2, 45, 132, 90,
    17, 119, 17, 100, 69, 200, 139, 248, 187, 146, 41, 194, 132, 132, 152, 237, 111, 80])
BZIP2_PAT = b"omnic bzip2 embedded test payload\n" * 200  # 6800B

LZIP_BLOB = bytes([
    76, 90, 73, 80, 1, 16, 0, 55, 155, 74, 29, 94, 104, 211, 11, 58, 125, 185, 252, 246,
    74, 56, 162, 230, 115, 234, 185, 188, 175, 91, 252, 244, 135, 251, 126, 45, 108, 152,
    201, 63, 0, 39, 136, 253, 224, 215, 86, 152, 102, 72, 229, 90, 216, 72, 39, 148, 171,
    44, 190, 234, 239, 8, 255, 254, 158, 4, 0, 45, 207, 166, 69, 228, 12, 0, 0, 0, 0, 0,
    0, 87, 0, 0, 0, 0, 0, 0, 0])
LZIP_PAT = b"omnic lzip embedded test payload\n" * 100  # 3300B

# 可产格式：omnic c -c <codec>（用默认档）
COMPRESSIBLE = ["zstd", "lz4", "xz", "lzma", "zlib", "zlib-stream", "raw-deflate",
                "zlib-ng", "libdeflate", "brotli", "snappy", "lzfse"]
# 无校验和的格式：静默错解为格式固有（raw deflate 裸流无 CRC/adler，
# 参考实现如 python zlib 同样不可检测）——计 warning 不计 fail
NO_CHECKSUM = {"raw-deflate"}


def make_payload(seed: int, size: int) -> bytes:
    rng = random.Random(seed)
    parts = []
    words = [b"omnic", b"codec", b"fuzz", b"payload", b"2026", b"stream", b"decode"]
    while sum(map(len, parts)) < size:
        kind = rng.random()
        if kind < 0.45:
            parts.append(b" ".join(rng.choice(words) for _ in range(rng.randint(3, 12))) + b"\n")
        elif kind < 0.7:
            parts.append(bytes(rng.randrange(256) for _ in range(rng.randint(16, 64))))
        else:
            v = rng.randrange(65536)
            parts.append(v.to_bytes(2, "little") * rng.randint(4, 20))
    return b"".join(parts)[:size]


def run_omnic(omnic: str, args, timeout=10):
    try:
        p = subprocess.run([omnic] + args, capture_output=True, timeout=timeout)
        return p.returncode, p.stdout
    except subprocess.TimeoutExpired:
        return "TIMEOUT", b""


def mutations(data: bytes, rng: random.Random, quick: bool):
    """生成 (label, bytes) 变体序列。"""
    n = len(data)
    out = []
    for frac in (0.05, 0.12, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.88, 0.95, 0.99):
        out.append((f"trunc{int(frac * 100)}", data[: int(n * frac)]))
    nflips = 25 if quick else 150
    for i in range(nflips):
        d = bytearray(data)
        pos = rng.randrange(n)
        d[pos] ^= 1 << rng.randrange(8)
        out.append((f"flip{i}", bytes(d)))
    for i, head in enumerate((b"\x00" * min(6, n), b"\xff" * min(6, n))):
        out.append((f"head{i}", head + data[min(6, n):]))
    out.append(("empty", b""))
    out.append(("one", data[:1]))
    out.append(("magiconly", data[: min(16, n)]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--omnic", default=None)
    ap.add_argument("--workdir", default=None)
    ap.add_argument("--quick", action="store_true", help="少量翻转，快速冒烟")
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()
    omnic = os.path.abspath(args.omnic or find_default_omnic())
    if not os.path.isfile(omnic):
        sys.exit(f"omnic binary not found: {omnic}")
    work = args.workdir or tempfile.mkdtemp(prefix="omnic_fuzz_")
    os.makedirs(work, exist_ok=True)
    rng = random.Random(args.seed)

    payload = make_payload(args.seed, 120 * 1024)
    golden_path = os.path.join(work, "golden.bin")
    with open(golden_path, "wb") as f:
        f.write(payload)

    # (format_label, artifact_path, golden bytes or None)
    cases = []
    for codec in COMPRESSIBLE:
        art = os.path.join(work, f"src_{codec}")
        rc, _ = run_omnic(omnic, ["c", "-k", "-c", codec, golden_path])
        # omnic 输出名 = golden.bin + native ext；产物路径未知 -> 直接搜
        prods = [p for p in os.listdir(work) if p.startswith("golden.bin.")]
        if rc != 0 or not prods:
            print(f"WARN: compress with {codec} failed (rc={rc}), skipping format")
            continue
        prod = os.path.join(work, prods[0])
        newart = os.path.join(work, f"good_{codec}" + os.path.splitext(prod)[1])
        os.replace(prod, newart)
        cases.append((f"{codec}", newart, payload))

    # decode-only：.Z 样本 + 内嵌 blob
    z = os.path.join(SRC_ROOT, "tests", "files", "z_text.Z")
    if os.path.isfile(z):
        cases.append(("compress(.Z)", os.path.abspath(z), None))
    bz = os.path.join(work, "good_bzip2.bz2")
    with open(bz, "wb") as f:
        f.write(BZIP2_BLOB)
    cases.append(("bzip2", bz, BZIP2_PAT))
    lz = os.path.join(work, "good_lzip.lz")
    with open(lz, "wb") as f:
        f.write(LZIP_BLOB)
    cases.append(("lzip", lz, LZIP_PAT))
    lv = os.path.join(work, "good_lzvn.lzvn")
    gen = None
    for sub in ("", "Release/"):
        for name in ("fuzz_lzvn_gen.exe", "fuzz_lzvn_gen"):
            p = os.path.join(SRC_ROOT, "build", sub + name)
            if os.path.isfile(p):
                gen = p
                break
        if gen:
            break
    if gen:
        rc = subprocess.run([gen, lv, os.path.join(work, "lzvn_ref.bin")], capture_output=True)
        if rc.returncode == 0:
            cases.append(("lzvn", lv, open(os.path.join(work, "lzvn_ref.bin"), "rb").read()))
        else:
            print("WARN: lzvn gen failed, skipping lzvn")
    else:
        print("WARN: fuzz_lzvn_gen not found in build/, skipping lzvn")

    # .Z 的 golden：先成功解压一次建立基准（文件带后缀避免产物同名冲突）
    for i, (label, art, gold) in enumerate(cases):
        if gold is None:
            ext = os.path.splitext(art)[1] or ".bin"
            cp = os.path.join(work, f"probe{i}{ext}")
            with open(cp, "wb") as f:
                f.write(open(art, "rb").read())
            rc, _ = run_omnic(omnic, ["d", "-k", cp])
            outp = cp[: cp.rfind(".")] if ext != ".bin" else cp
            if rc == 0 and os.path.isfile(outp):
                cases[i] = (label, art, open(outp, "rb").read())
            else:
                cases[i] = (label, art, b"\x00PROBE-FAILED\x00")  # 无基准：跳过产物比对

    total = fails = 0
    fail_detail = []
    warn_detail = []
    t0 = time.time()
    outdir = os.path.join(work, "out")
    os.makedirs(outdir, exist_ok=True)
    print(f"fuzz: {len(cases)} formats, workdir {work}")
    for label, art, gold in cases:
        data = open(art, "rb").read()
        ext = os.path.splitext(art)[1] or ".bin"
        stats = {"pass": 0, "err": 0, "silent": 0, "crash": 0, "hang": 0, "recovered": 0}
        for mlabel, mdata in mutations(data, rng, args.quick):
            save = False
            casef = os.path.join(work, "case" + ext)
            with open(casef, "wb") as f:
                f.write(mdata)
            expected_out = os.path.join(outdir, "case")
            if os.path.isfile(expected_out):
                os.remove(expected_out)
            rc, _ = run_omnic(omnic, ["d", "-k", "-o", outdir, casef])
            total += 1
            if rc == "TIMEOUT":
                stats["hang"] += 1
                fail_detail.append((label, mlabel, "HANG"))
                save = True
            elif isinstance(rc, int) and (rc < 0 or rc >= 0x80000000):
                stats["crash"] += 1
                fail_detail.append((label, mlabel, f"CRASH rc={rc}"))
                save = True
            elif rc == 0:
                got = open(expected_out, "rb").read() if os.path.isfile(expected_out) else b""
                if gold and got == gold:
                    stats["recovered"] += 1
                elif gold is None or gold == b"\x00PROBE-FAILED\x00":
                    stats["pass"] += 1  # 无基准，只验证不崩溃
                else:
                    stats["silent"] += 1
                    if label in NO_CHECKSUM:
                        warn_detail.append((label, mlabel, "SILENT (no-checksum format)"))
                    else:
                        fail_detail.append((label, mlabel, "SILENT_CORRUPTION"))
                    save = label not in NO_CHECKSUM
            else:
                stats["err"] += 1
            if save:
                keep = os.path.join(work, f"repro_{mlabel}_{label.replace('(', '').replace(')', '')}{ext}")
                with open(keep, "wb") as f:
                    f.write(mdata)
        ok = (stats["crash"] == 0 and stats["hang"] == 0
              and (stats["silent"] == 0 or label in NO_CHECKSUM))
        fails += 0 if ok else 1
        note = f"  silent={stats['silent']}" if label in NO_CHECKSUM else ""
        print(f"  {label:16s} err={stats['err']:3d} recovered={stats['recovered']:2d} "
              f"silent={stats['silent']} crash={stats['crash']} hang={stats['hang']}"
              f"{'  <-- FAIL' if not ok else ''}{note}")

    print(f"\nfuzz done: {total} cases in {time.time() - t0:.1f}s, "
          f"{fails}/{len(cases)} formats failed")
    if warn_detail:
        print("warnings (format-inherent, no-checksum formats):")
        for label, m, kind in warn_detail[:5]:
            print(f"  {label:16s} {m:12s} {kind}")
    if fail_detail:
        print("failure detail (first 20):")
        for label, m, kind in fail_detail[:20]:
            print(f"  {label:16s} {m:12s} {kind}")
        # 保留一个失败样本供复现
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()

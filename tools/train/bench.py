"""训练矩阵编排器（PHASE2 M4，SW-TRAIN-001 复用 / SW-TRAIN-004 可追溯）。

corpora 结构: <corpora>/<class>/<dist>/   目录内文件按名排序拼接为一个语料（每分布 >=20MiB）
输出: <out>/matrix__<class>__<dist>.csv + manifest.json（sha256/git sha/时间戳）

用法:
  python tools/train/bench.py --out build/train/<run> [--corpora tools/train/corpora]
                              [--bench build/omnic_bench.exe] [--classes code,ntext] [--force]

注意: bench 的临时文件固定名于其 cwd——串行执行且 cwd 设为输出目录（隔离踩踏）。
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--corpora', default='tools/train/corpora')
    ap.add_argument('--out', required=True, help='输出目录（如 build/train/20261031_dc76738）')
    ap.add_argument('--bench', default=None)
    ap.add_argument('--classes', default=None, help='逗号分隔；缺省 = corpora 下全部')
    ap.add_argument('--force', action='store_true', help='忽略缓存重跑')
    args = ap.parse_args()

    bench = args.bench or ('build/omnic_bench.exe' if os.name == 'nt' else 'build/omnic_bench')
    if not Path(bench).exists():
        sys.exit('bench not found: %s' % bench)
    bench = str(Path(bench).resolve())

    corpora = Path(args.corpora)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    manifest_path = out / 'manifest.json'
    manifest = []
    if manifest_path.exists() and not args.force:
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))

    classes = sorted(p.name for p in corpora.iterdir() if p.is_dir()) if corpora.exists() else []
    if args.classes:
        keep = set(args.classes.split(','))
        classes = [c for c in classes if c in keep]

    git = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'],
                         capture_output=True, text=True).stdout.strip()

    for klass in classes:
        dists = sorted(p.name for p in (corpora / klass).iterdir() if p.is_dir())
        for dist in dists:
            files = sorted(f for f in (corpora / klass / dist).iterdir() if f.is_file())
            if not files:
                print('skip empty %s/%s' % (klass, dist))
                continue
            csv = out / ('matrix__%s__%s.csv' % (klass, dist))
            cached = any(e['class'] == klass and e['dist'] == dist for e in manifest)
            if cached and csv.exists() and not args.force:
                print('cached %s/%s' % (klass, dist))
                continue
            cmd = [bench, '--csv', csv.name]
            for f in files:
                cmd += ['--corpus', str(f.resolve())]
            t0 = time.time()
            r = subprocess.run(cmd, cwd=str(out), capture_output=True, text=True, timeout=3600)
            if r.returncode != 0:
                sys.exit('bench failed %s/%s: %s' % (klass, dist, r.stderr[-400:]))
            entry = {'class': klass, 'dist': dist, 'csv': csv.name, 'git_sha': git,
                     'files': [{'name': f.name, 'bytes': f.stat().st_size,
                                'sha256': sha256_file(f)} for f in files]}
            manifest = [e for e in manifest
                        if not (e['class'] == klass and e['dist'] == dist)] + [entry]
            manifest_path.write_text(json.dumps(manifest, indent=1), encoding='utf-8')
            print('ran %s/%s: %d files, %.0fs' % (klass, dist, len(files), time.time() - t0))

    print('done: %d dists -> %s' % (len(manifest), out))


if __name__ == '__main__':
    main()

"""fit: 矩阵 CSV -> 类级聚合 -> 前沿 -> 四档选择（PHASE2 §3.6 预注册规则）。

用法:
  python tools/train/fit.py --train <dir> [--classes a,b] [--exclude-dist class:dist]...
  python tools/train/fit.py --train <dir> --loo          # 每分布轮流留出（给 verify.py J2 用）

输出: <train>/fit.json（普通模式）或 <train>/fit_loo.json（--loo）:
  {"aggregate": "geomean(vc,vd)/mean(rho)", "classes": {<class>: {
     "dists": [...], "points": {<cfg>: {vc,vd,rho}}, "frontier": [...],
     "pick": {"fc": {...codec,level,codec2,level2}, "fd":..., "bal":..., "br":...}}}}
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import pareto


def fit_classes(train_dir, classes=None, exclude=None):
    manifest = json.loads((train_dir / 'manifest.json').read_text(encoding='utf-8'))
    groups = {}
    for e in manifest:
        if classes and e['class'] not in classes:
            continue
        if exclude and '%s:%s' % (e['class'], e['dist']) in exclude:
            continue
        groups.setdefault(e['class'], []).append(e)

    out = {}
    for klass, entries in sorted(groups.items()):
        per_dist = [pareto.load_matrix(train_dir / e['csv']) for e in entries]
        names = {p['name'] for m in per_dist for p in m}
        # 补齐缺失配置（bench 版本变动等），缺失档不参与该分布聚合
        points = pareto.aggregate([m for m in per_dist if m])
        pick = pareto.pick_profiles(points)
        out[klass] = {
            'dists': [e['dist'] for e in entries],
            'n_configs': len(names),
            'points': {p['name']: {k: round(p[k], 4) for k in ('vc', 'vd', 'rho')}
                       for p in points},
            'frontier': sorted(p['name'] for p in pareto.frontier(points)),
            'pick': pick,
        }
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--train', required=True)
    ap.add_argument('--classes', default=None)
    ap.add_argument('--exclude-dist', action='append', default=[],
                    help='class:dist，可重复（LOO 由 --loo 自动做）')
    ap.add_argument('--loo', action='store_true')
    args = ap.parse_args()
    train = Path(args.train)
    classes = set(args.classes.split(',')) if args.classes else None

    if args.loo:
        result = {}
        base = fit_classes(train, classes)
        for klass, info in base.items():
            for dist in info['dists']:
                sub = fit_classes(train, {klass}, {'%s:%s' % (klass, dist)})
                result['%s:%s' % (klass, dist)] = sub.get(klass, {}).get('pick')
        (train / 'fit_loo.json').write_text(json.dumps(result, indent=1), encoding='utf-8')
        print('LOO: %d folds -> %s' % (len(result), train / 'fit_loo.json'))
    else:
        result = {'aggregate': 'geomean(vc,vd)/mean(rho) (PHASE2 3.6)',
                  'classes': fit_classes(train, classes, set(args.exclude_dist))}
        (train / 'fit.json').write_text(json.dumps(result, indent=1), encoding='utf-8')
        for klass, info in result['classes'].items():
            picks = ', '.join('%s=%s/%d' % (p, v['codec'], v['level'])
                              for p, v in sorted(info['pick'].items()))
            print('%s [%s]: %s' % (klass, ','.join(info['dists']), picks))
        print('-> %s' % (train / 'fit.json'))


if __name__ == '__main__':
    main()

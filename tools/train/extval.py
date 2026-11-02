"""extval: 外部验证（M6 J-EX）——真实世界文件上的表值 regret 对照。

对照三表: 手工初值表（policy_table.gen.c 现值）、fit 训练表、随机基线（类前沿
任选）。regret 口径与 LOO 一致（速度相对 %、rho 绝对 pp、STRICT 速度 1.10 /
rho 1.005 容差）。

用法:
  python tools/train/extval.py --mat <dir>            # dir 含 matrix_ext__*.csv
  python tools/train/extval.py --mat <dir> --files a=ntext,b=photo ...

文件名到类的映射: 默认按 `name=class` 参数; 无映射时从 CSV 行数/文件名猜不
做（必须显式给，防错配）。
"""
import argparse
import csv
import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import pareto

# 一期手工表主值（policy_table.gen.c 手工初值，训练前的基线）
MANUAL = {
    'ntext':      {'fc': ('lz4', 0), 'fd': ('zstd', 3), 'bal': ('zstd', 8), 'br': ('xz', 9)},
    'code':       {'fc': ('zstd', 3), 'fd': ('zstd', 3), 'bal': ('zstd', 8), 'br': ('xz', 9)},
    'exec':       {'fc': ('lz4', 0), 'fd': ('zstd', 3), 'bal': ('zstd', 8), 'br': ('xz', 9)},
    'structured': {'fc': ('lz4', 0), 'fd': ('zstd', 3), 'bal': ('zstd', 8), 'br': ('xz', 9)},
    'graphic':    {'fc': ('zstd', -3), 'fd': ('zstd', 3), 'bal': ('brotli', 5), 'br': ('brotli', 11)},
    'photo':      {'fc': ('zstd', -3), 'fd': ('zstd', 3), 'bal': ('zstd', 8), 'br': ('xz', 6)},
    'synth_pic':  {'fc': ('brotli', 9), 'fd': ('brotli', 9), 'bal': ('zstd', 1), 'br': ('brotli', 9)},
}


def load_matrix(path):
    return {(r['codec'], int(r['level'])): (float(r['ratio_pct']), float(r['comp_base_mbs']),
                                            float(r['dec_base_mbs']))
            for r in csv.DictReader(open(path))}


def regret_of(cfg, m, prof):
    t = m.get(cfg)
    if t is None:
        return (99.0, 99.0)
    if prof == 'br':
        doms = [v for v in m.values() if v[0] * pareto.STRICT_RHO < t[0]]
        return (0.0, max((t[0] - v[0] for v in doms), default=0.0))
    dims = {'fc': (1,), 'fd': (2,), 'bal': (1, 2)}[prof]
    vr, rr = 0.0, 0.0
    for k, v in m.items():
        if k == cfg:
            continue
        ok = v[0] <= t[0] + 1e-9
        strict = False
        for di in dims:
            ok = ok and v[di] >= t[di] * 0.9999
            if v[di] > t[di] * 1.10:
                strict = True
        if ok and strict:
            rr = max(rr, t[0] - v[0])
            for di in dims:
                vr = max(vr, v[di] / t[di] - 1)
    return (vr, rr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mat', required=True, help='外部矩阵目录（matrix_ext__*.csv）')
    ap.add_argument('--fit', default='build/train/m5/fit.json')
    ap.add_argument('--files', required=True, nargs='+',
                    help='name=class 映射，如 war_and_peace=ntext photo_0=photo')
    args = ap.parse_args()
    mat = Path(args.mat)
    fit = json.loads(Path(args.fit).read_text(encoding='utf-8'))['classes']

    mapping = {}
    for f in args.files:
        name, klass = f.split('=')
        mapping[name] = klass

    print('%-18s %-10s | %-14s | %-14s | %-14s' % ('file', 'class', 'manual', 'fit', 'rand(mean)'))
    agg = {'manual': [], 'fit': [], 'rand': []}
    for name, klass in sorted(mapping.items()):
        p = mat / ('matrix_ext__%s.csv' % name)
        if not p.exists():
            print('%-18s 缺矩阵，跳过' % name)
            continue
        m = load_matrix(p)
        info = fit[klass]
        frs_all = []
        for prof in ['fc', 'fd', 'bal', 'br']:
            fitcfg = (info['pick'][prof]['codec'], info['pick'][prof]['level'])
            fm = regret_of(MANUAL[klass][prof], m, prof)
            ff = regret_of(fitcfg, m, prof)
            agg['manual'].append(fm)
            agg['fit'].append(ff)
            for fname in info['frontier']:
                codec, lvl = fname.split('/')
                frs_all.append(regret_of((codec, int(lvl)), m, prof))
        agg['rand'].append((statistics.mean(x[0] for x in frs_all),
                            statistics.mean(x[1] for x in frs_all)))
        mv = statistics.mean(x[0] for x in agg['manual'][-4:])
        mr = statistics.mean(x[1] for x in agg['manual'][-4:])
        fv = statistics.mean(x[0] for x in agg['fit'][-4:])
        frr = statistics.mean(x[1] for x in agg['fit'][-4:])
        print('%-18s %-10s | v%5.1f%% r%5.2f | v%5.1f%% r%5.2f | v%6.1f%% r%5.2f' % (
            name, klass, mv * 100, mr, fv * 100, frr, agg['rand'][-1][0] * 100, agg['rand'][-1][1]))

    print()
    for k in ['manual', 'fit', 'rand']:
        vs = [x[0] for x in agg[k]]
        rs = [x[1] for x in agg[k]]
        print('%-8s: v_regret mean %6.1f%% max %7.1f%% | rho_regret mean %5.2f pp max %6.2f pp (n=%d)' % (
            k, statistics.mean(vs) * 100, max(vs) * 100, statistics.mean(rs), max(rs), len(vs)))
    worst = sorted(agg['fit'], key=lambda x: -x[1])[:3]
    print('fit worst rho regrets: %s pp' % ', '.join('%.2f' % x[1] for x in worst))


if __name__ == '__main__':
    main()

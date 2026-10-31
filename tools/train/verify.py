"""verify: J1/J2 门禁自动化（PHASE2 §2.3 预注册）。

J2: LOO——每 (class, dist) 用 LOO pick 在留出分布矩阵上查实质支配者；门禁 = 0。
J1: R4 六分布复测——用 fit（或现行表）在 R4 语料矩阵上查支配计数；门禁 <= 6。

用法:
  python tools/train/verify.py --train <dir>                        # J2（需 fit.py --loo 已跑）
  python tools/train/verify.py --train <dir> --fit <fit.json>      # 附加：当前 fit 的支配计数
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import pareto

PROFILE_DIMS = {'fc': ('vc',), 'fd': ('vd',), 'bal': ('vc', 'vd'), 'br': ()}


def dom_count(cfg, matrix, kmax):
    """表配置在矩阵上的实质支配数（br: 纯 rho 维支配计数）。"""
    t = next((p for p in matrix if p['codec'] == cfg[0] and p['level'] == cfg[1]), None)
    if t is None:
        return 1, ['<missing-from-matrix>']  # 表配置不在矩阵 = 无法验证，按失败计
    kmin = ('rho',)
    doms = [b['name'] for b in matrix
            if b is not t and pareto.dominates(b, t, kmax, kmin)]
    return len(doms), doms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--train', required=True)
    ap.add_argument('--fit', default=None, help='附带报告该 fit 各类的支配计数')
    args = ap.parse_args()
    train = Path(args.train)
    rc = 0

    loo_path = train / 'fit_loo.json'
    if loo_path.exists():
        loo = json.loads(loo_path.read_text(encoding='utf-8'))
        manifest = json.loads((train / 'manifest.json').read_text(encoding='utf-8'))
        by_cd = {(':%s:%s' % (e['class'], e['dist'])): e for e in manifest}
        print('== J2: LOO 支配计数（门禁 = 0） ==')
        for key, pick in sorted(loo.items()):
            if not pick:
                continue
            klass, dist = key.split(':', 2)[1:3] if key.count(':') == 2 else (None, None)
            e = by_cd.get(key if key.startswith(':') else ':' + key)
            if e is None:
                e = next((m for m in manifest
                          if key == '%s:%s' % (m['class'], m['dist'])), None)
            if e is None:
                print('  %s: 找不到对应矩阵，跳过' % key)
                continue
            matrix = pareto.load_matrix(train / e['csv'])
            bad = []
            for prof, cfg in sorted(pick.items()):
                kmax = PROFILE_DIMS.get(prof, ('vc', 'vd'))
                n, doms = dom_count((cfg['codec'], cfg['level']), matrix, kmax)
                if prof == 'br':
                    t = next((p for p in matrix
                              if p['codec'] == cfg['codec'] and p['level'] == cfg['level']), None)
                    if t is None:
                        n, doms = 1, ['<missing>']
                    else:
                        doms = [b['name'] for b in matrix if b['rho'] < t['rho']]
                        n = len(doms)
                if n:
                    bad.append('%s(%d): %s' % (prof, n, ','.join(doms[:3])))
            status = 'PASS' if not bad else 'FAIL'
            rc |= 1 if bad else 0
            print('  %s %s' % (key, status))
            for b in bad:
                print('      %s' % b)
    else:
        print('J2: 无 fit_loo.json（先跑 fit.py --loo），跳过')

    if args.fit:
        fit = json.loads(Path(args.fit).read_text(encoding='utf-8'))
        manifest = json.loads((train / 'manifest.json').read_text(encoding='utf-8'))
        print('\n== 当前 fit 在各训练分布上的支配计数（参考，非门禁） ==')
        for klass, info in sorted(fit.get('classes', fit).items()):
            for e in [m for m in manifest if m['class'] == klass]:
                matrix = pareto.load_matrix(train / e['csv'])
                parts = []
                for prof, cfg in sorted(info['pick'].items()):
                    kmax = PROFILE_DIMS.get(prof, ('vc', 'vd'))
                    n, doms = dom_count((cfg['codec'], cfg['level']), matrix, kmax)
                    if n:
                        parts.append('%s:%d' % (prof, n))
                print('  %s/%s: %s' % (klass, e['dist'], ','.join(parts) or '0'))

    sys.exit(rc)


if __name__ == '__main__':
    main()

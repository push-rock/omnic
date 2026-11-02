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

# 提议的 M6 门禁形式（待批复）：有界 regret——聚合 LOO 支配是分布异质性的
# 本质后果，零支配只在类内同质时可达。界值取当前损失分布的 natural break。
REGRET_RHO_PP = 1.0
REGRET_V_PCT = 50.0


def dom_regret(cfg, matrix, kmax, prof):
    """表配置在矩阵上的实质支配者 + regret（速度相对 %、ρ 绝对 pp）。"""
    t = next((p for p in matrix if p['codec'] == cfg[0] and p['level'] == cfg[1]), None)
    if t is None:
        return 1, ['<missing-from-matrix>'], 0.0, 0.0
    doms, v_reg, rho_reg = [], 0.0, 0.0
    for b in matrix:
        if b is t:
            continue
        if prof == 'br':
            if b['rho'] * pareto.STRICT_RHO < t['rho']:
                doms.append(b['name'])
                rho_reg = max(rho_reg, (t['rho'] - b['rho']) / 1.0)
            continue
        if not pareto.dominates(b, t, kmax, ('rho',)):
            continue
        doms.append(b['name'])
        rho_reg = max(rho_reg, t['rho'] - b['rho'])
        for k in kmax:
            v_reg = max(v_reg, b[k] / t[k] - 1.0)
    return len(doms), doms, v_reg, rho_reg


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
            over = []  # 超提议 regret 界（REGRET_RHO_PP / REGRET_V_PCT）的档
            for prof, cfg in sorted(pick.items()):
                kmax = PROFILE_DIMS.get(prof, ('vc', 'vd'))
                n, doms, v_reg, rho_reg = dom_regret((cfg['codec'], cfg['level']),
                                                     matrix, kmax, prof)
                if not n:
                    continue
                bad.append('%s(%d): %s' % (prof, n, ','.join(doms[:3])))
                # regret 量化（速度相对 %、ρ 绝对 pp）
                if v_reg > 0.005 or rho_reg > 0.005:
                    bad[-1] += '  [regret: v +%.0f%%, rho -%.2f pp]' % (v_reg * 100, rho_reg)
                if rho_reg > REGRET_RHO_PP or v_reg * 100 > REGRET_V_PCT:
                    over.append(prof)
            status = 'PASS' if not bad else ('OVER' if over else 'FAIL')
            rc |= 1 if over else 0
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
                    n, _, _, _ = dom_regret((cfg['codec'], cfg['level']), matrix, kmax, prof)
                    if n:
                        parts.append('%s:%d' % (prof, n))
                print('  %s/%s: %s' % (klass, e['dist'], ','.join(parts) or '0'))

    sys.exit(rc)


if __name__ == '__main__':
    main()

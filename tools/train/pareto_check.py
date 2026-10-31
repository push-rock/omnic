"""决策表表值的帕累托前沿检查（PHASE2_DECIDER_DESIGN M4 fit 的雏形）。

输入: bench 全矩阵 CSV（omnic_bench --corpus <语料> --csv out.csv）
输出: 各档位经验前沿 + 表值判定（在前沿/被支配/档位错配）

用法: python tools/train/pareto_check.py <matrix.csv> <codec/level> <codec/level> <codec/level> <codec/level>
      四个表值依次为 FC/FD/BAL/BR（如 zstd/3 zstd/3 zstd/8 xz/9）

判定口径（与 J1 一致）：
  - 基础支配: a ⊳ b ⟺ 全维不劣且至少一维严格（速度维 >10% 才计入严格优势）
  - FC 前沿 = (vc, rho) 非支配集; FD = (vd, rho); BAL = 三维对数理想点距离 argmin;
    BR = argmin rho
"""
import csv
import math
import sys

STRICT_SPEED = 1.10  # 速度维严格优势阈值（J1 口径）


def load(path):
    rows = []
    with open(path, newline='', encoding='utf-8') as f:
        for r in csv.DictReader(f):
            rows.append({
                'name': '%s/%s' % (r['codec'], r['level']),
                'rho': float(r['ratio_pct']),
                'vc': float(r['comp_base_mbs']),
                'vd': float(r['dec_base_mbs']),
            })
    return rows


def dominates(a, b, kmax, kmin):
    """速度维(kmax)需 >10% 优势才计入严格；rho 维(kmin)任何严格都计入。"""
    ge = all(a[k] >= b[k] for k in kmax) and all(a[k] <= b[k] for k in kmin)
    if not ge:
        return False
    strict = any(a[k] >= b[k] * STRICT_SPEED for k in kmax) or any(a[k] < b[k] for k in kmin)
    return strict


def front(rs, kmax, kmin):
    return [a for a in rs
            if not any(b is not a and dominates(b, a, kmax, kmin) for b in rs)]


def main():
    if len(sys.argv) != 6:
        print(__doc__)
        sys.exit(2)
    rows = load(sys.argv[1])
    fc_t, fd_t, bal_t, br_t = sys.argv[2:6]
    by = {r['name']: r for r in rows}
    N = len(rows)

    def get(name):
        if name not in by:
            print('!! %s 不在矩阵（bench 档位缺口，先补 lv_* 数组）' % name)
            return None
        return by[name]

    for title, kmax, kmin, tname in (('FC', ['vc'], ['rho'], fc_t),
                                     ('FD', ['vd'], ['rho'], fd_t)):
        fr = front(rows, kmax, kmin)
        print('== %s 前沿 (%d/%d): %s' % (title, len(fr), N,
                                          ', '.join(sorted(x['name'] for x in fr))))
        t = get(tname)
        if t is None:
            continue
        if t in fr:
            print('   表值 %s: 在前沿' % tname)
        else:
            dom = [b['name'] for b in rows if b is not t and dominates(b, t, kmax, kmin)]
            print('   表值 %s: 被支配 -> %s' % (tname, dom))

    # BAL: 三维对数理想点（对称 w=1/3）
    vc_max = max(r['vc'] for r in rows)
    vd_max = max(r['vd'] for r in rows)
    rho_min = min(r['rho'] for r in rows)
    for r in rows:
        g = (math.log(vc_max / r['vc']), math.log(vd_max / r['vd']), math.log(r['rho'] / rho_min))
        r['d'] = math.sqrt((g[0] ** 2 + g[1] ** 2 + g[2] ** 2) / 3.0)
    bal = sorted(rows, key=lambda x: x['d'])
    print('== BAL 对称距离 top3: %s' %
          ', '.join('%s(d=%.3f)' % (x['name'], x['d']) for x in bal[:3]))
    t = get(bal_t)
    if t is not None:
        rank = bal.index(t) + 1
        print('   表值 %s: 名次 %d/%d, d=%.3f' % (bal_t, rank, N, t['d']))

    br = sorted(rows, key=lambda x: x['rho'])
    print('== BR argmin rho top3: %s' %
          ', '.join('%s(%.2f%%)' % (x['name'], x['rho']) for x in br[:3]))
    t = get(br_t)
    if t is not None:
        rank = br.index(t) + 1
        print('   表值 %s: rho 名次 %d/%d (%.2f%%)' % (br_t, rank, N, t['rho']))


if __name__ == '__main__':
    main()

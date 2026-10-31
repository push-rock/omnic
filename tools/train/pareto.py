"""pareto: 前沿与档位规则的共享实现（PHASE2_DECIDER_DESIGN §1.3/§3.6 预注册）。

支配口径（与 J1 一致）: a ⊳ b ⟺ 全维不劣 ∧ 至少一维严格（速度维 >10% 优势才计入严格）。
聚合口径: v = geomean_i(v_i)（速度比率语义）; rho = mean_i(rho_i)（等权, 与 LOO 单位一致）。
"""
import csv
import math

STRICT_SPEED = 1.10


def load_matrix(path):
    """bench CSV -> [{'name','codec','level','rho','vc','vd'}]"""
    out = []
    with open(path, newline='', encoding='utf-8') as f:
        for r in csv.DictReader(f):
            out.append({'name': '%s/%s' % (r['codec'], r['level']),
                        'codec': r['codec'], 'level': int(r['level']),
                        'rho': float(r['ratio_pct']),
                        'vc': float(r['comp_base_mbs']),
                        'vd': float(r['dec_base_mbs'])})
    return out


def dominates(a, b, kmax=('vc', 'vd'), kmin=('rho',)):
    ge = all(a[k] >= b[k] for k in kmax) and all(a[k] <= b[k] for k in kmin)
    if not ge:
        return False
    strict = any(a[k] >= b[k] * STRICT_SPEED for k in kmax) or any(a[k] < b[k] for k in kmin)
    return strict


def frontier(points, kmax=('vc', 'vd'), kmin=('rho',)):
    return [a for a in points
            if not any(b is not a and dominates(b, a, kmax, kmin) for b in points)]


def aggregate(matrices):
    """多分布矩阵 [[point]] -> 类级聚合点（按配置名对齐；任一分布缺失的配置不参与聚合）。"""
    names, seen = [], set()
    for m in matrices:
        for p in m:
            if p['name'] not in seen:
                seen.add(p['name'])
                names.append(p['name'])
    out = []
    for n in names:
        vs = [p for m in matrices for p in m if p['name'] == n]
        if len(vs) < len(matrices):
            continue
        out.append({'name': n, 'codec': vs[0]['codec'], 'level': vs[0]['level'],
                    'vc': math.exp(sum(math.log(v['vc']) for v in vs) / len(vs)),
                    'vd': math.exp(sum(math.log(v['vd']) for v in vs) / len(vs)),
                    'rho': sum(v['rho'] for v in vs) / len(vs)})
    return out


def bal_distance(p, front):
    """预注册式：前沿区间对数归一化 x̂ = ln(x/x_min)/ln(x_max/x_min)，d = √[(1-v̂c)²+(1-v̂d)²+ρ̂²]。"""
    def norm(val, lo, hi):
        return 0.0 if hi <= lo else math.log(val / lo) / math.log(hi / lo)
    vcs = [x['vc'] for x in front]
    vds = [x['vd'] for x in front]
    rhos = [x['rho'] for x in front]
    vc_n = norm(p['vc'], min(vcs), max(vcs))
    vd_n = norm(p['vd'], min(vds), max(vds))
    rho_n = norm(p['rho'], min(rhos), max(rhos))
    return math.sqrt((1 - vc_n) ** 2 + (1 - vd_n) ** 2 + rho_n ** 2)


def pick_profiles(points):
    """四档规则（§3.6 预注册）。输入类级聚合点，输出 {profile: {codec, level, codec2, level2}}。"""
    fr = frontier(points)
    if not fr:
        return {}
    by = {p['name']: p for p in points}
    pick = {}

    # FC: 前沿中 rho <= 2*rho_min 里 argmax vc；第二候选 = 前沿 argmax rho（vc >= 100MB/s）
    rho_min = min(x['rho'] for x in fr)
    band = [x for x in fr if x['rho'] <= 2.0 * rho_min]
    fc = max(band, key=lambda x: x['vc']) if band else None
    if fc:
        pick['fc'] = {'codec': fc['codec'], 'level': fc['level']}
        c2 = max(fr, key=lambda x: x['rho'])
        if c2['name'] != fc['name'] and c2['vc'] >= 100.0:
            pick['fc']['codec2'] = c2['codec']
            pick['fc']['level2'] = c2['level']

    # FD: 与 FC 对称——前沿中 rho <= 2*rho_min 里 argmax vd
    # （原 0.9*vd_max 相对门槛在 snappy 解压独高的分布下 band 退化为单点，
    #  冒烟暴露；对称化后两档规则同构，见文档 §3.6 实现期修正 #5）
    fd = max(band, key=lambda x: x['vd']) if band else None
    if fd:
        pick['fd'] = {'codec': fd['codec'], 'level': fd['level']}

    # BAL: 前沿区间归一化理想点距离 argmin
    bal = min(fr, key=lambda x: bal_distance(x, fr))
    pick['bal'] = {'codec': bal['codec'], 'level': bal['level']}

    # BR: argmin rho（并列 <0.5% 取 vd 高者）
    br = min(fr, key=lambda x: (x['rho'], -x['vd']))
    near = [x for x in fr if x['rho'] < br['rho'] * 1.005]
    br = max(near, key=lambda x: x['vd']) if len(near) > 1 else br
    pick['br'] = {'codec': br['codec'], 'level': br['level']}
    return pick


def dominated_by(table_cfg, matrix):
    """表配置 (codec, level) 在矩阵 points 上的实质支配者列表（J1/J2 复用）。"""
    t = None
    for p in matrix:
        if p['codec'] == table_cfg[0] and p['level'] == table_cfg[1]:
            t = p
            break
    if t is None:
        return ['<missing-from-matrix>']
    return [b['name'] for b in matrix
            if b is not t and dominates(b, t)]

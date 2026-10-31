"""gen: fit.json -> policy_table.gen.c（表全量生成，含 codec2 列）。

用法:
  python tools/train/gen.py --fit build/train/<run>/fit.json
                            [--out src/policy/policy_table.gen.c] [--check]

规则:
  - fit.json 覆盖对应类行；未覆盖的类保留现表值（增量重训，M6 前的 corpora 未建全时使用）
  - ALREADY_COMPRESSED / RANDOM 固定 skip 行（假设 A1）
  - reason 保持一期风格（无数字，CLI 展示稳定）；生成来源记入文件头注释
"""
import argparse
import json
import re
import sys
import time
from pathlib import Path

CLS = [
    ('ntext', 'NTEXT', 'natural text'),
    ('code', 'CODE', 'source code'),
    ('exec', 'EXEC', 'exec'),
    ('structured', 'STRUCTURED', 'structured'),
    ('graphic', 'GRAPHIC', 'graphic'),
    ('photo', 'PHOTO', 'photo'),
    ('already_compressed', 'ALREADY_COMPRESSED', 'already compressed'),
    ('random', 'RANDOM', 'high entropy >=7.5 bits/byte'),
    ('unknown', 'UNKNOWN', 'unknown'),
]
PROFILES = [('FAST_COMPRESS', 'fc', 'fastest compression'),
            ('FAST_DECOMPRESS', 'fd', 'fast decompress'),
            ('BALANCED', 'bal', 'balanced'),
            ('BEST_RATIO', 'br', 'best ratio')]
SKIP_ROW = ('%s, OMNIC_PROFILE_%s, OMNIC_SKIP_CODEC, 0, '
            '"%s: skip (use --force)", NULL, 0')

ROW_RE = re.compile(
    r'\{\s*OMNIC_CLS_(\w+),\s*OMNIC_PROFILE_(\w+),\s*"(\w+)",\s*(-?\d+),\s*"([^"]*)"'
    r'(?:,\s*(?:"(\w+)"|NULL),\s*(-?\d+))?\s*\}')


def parse_current(path):
    """现表 -> {(class_tag, profile_tag): (codec, level, reason, codec2|None, level2)}"""
    rows = {}
    for m in ROW_RE.finditer(Path(path).read_text(encoding='utf-8')):
        cls, prof, codec, lvl, reason, c2, l2 = m.groups()
        rows[(cls.lower(), prof.lower().replace('fast_compress', 'fc')
              .replace('fast_decompress', 'fd').replace('balanced', 'bal')
              .replace('best_ratio', 'br'))] = (codec, int(lvl), reason,
                                                c2, int(l2) if l2 else 0)
    return rows


def level_disp(codec, lvl):
    if codec == 'zstd' and lvl < 0:
        return 'zstd --fast=%d' % lvl
    if lvl == 0 and codec in ('lz4', 'lzfse', 'snappy'):
        return codec
    return '%s -%d' % (codec, lvl)


def build_rows(baseline, fit):
    out = []
    for tag, enum, desc in CLS:
        for penum, ptag, psuf in PROFILES:
            fitted = (fit or {}).get(tag, {}).get('pick', {}).get(ptag)
            if tag == 'already_compressed':
                reason = 'already compressed: skip (use --force)'
            elif tag == 'random':
                reason = 'high entropy >=7.5 bits/byte: skip (use --force)'
            elif fitted:
                codec, lvl = fitted['codec'], fitted['level']
                reason = '%s: %s (%s)' % (desc, level_disp(codec, lvl), psuf)
            else:
                old = baseline.get((tag, ptag))
                if old is None:
                    sys.exit('baseline 缺行 %s/%s（现表解析失败？）' % (tag, ptag))
                codec, lvl, reason = old[0], old[1], old[2]
            skip = tag in ('already_compressed', 'random')
            c2 = l2 = None
            if fitted and 'codec2' in fitted:
                c2, l2 = fitted['codec2'], fitted['level2']
            elif not fitted and baseline.get((tag, ptag)) and baseline[(tag, ptag)][3]:
                c2, l2 = baseline[(tag, ptag)][3], baseline[(tag, ptag)][4]
            tail = '' if c2 is None else ', "%s", %d' % (c2, l2)
            if not tail:
                tail = ', NULL, 0'  # 全字段显式初始化（-Wmissing-field-initializers）
            row = ('{OMNIC_CLS_%s, OMNIC_PROFILE_%s, %s, %d, "%s"%s},' %
                   (enum, penum, 'OMNIC_SKIP_CODEC' if skip else '"%s"' % codec,
                    0 if skip else lvl, reason, tail))
            out.append((tag, ptag, row))
    return out


HEADER = '''/* 决策表（SW-POL-002）。%s
 * 结构: 8 类 x 4 档 = 32 行；粗类 TEXT/MEDIA_RAW 经 gamma 细分后不进表。
 * 规则与支配口径见 docs/PHASE2_DECIDER_DESIGN.md（本地）；生成勿手改，
 * 手动调整须在表尾登记（SW-TRAIN-002 由 tools/train 生成）。 */
#include "policy/policy.h"

const omnic_policy_entry omnic_policy_entries[] = {
'''

FOOTER = '''};

/* 手动修改登记：
 * 2026-10-30（一期）TEXT/BEST_RATIO: xz -9 -> brotli -11，源码合集实测；
 * 2026-10-31（二期 M2）手工初值表（8 类），R4 支配实证 + feature_probe 校准。
 * 2026-10-31（二期 M4）tools/train 训练管线接管生成（fit 规则预注册见文档）。
 * zlib/zlib-ng/libdeflate/snappy/lzfse 在一期全组合被压制不进自动选路——
 * §3.7 F2 反驳了该结论，libdeflate 已重新纳入训练候选（bench 矩阵含其全部档位）。 */
const size_t omnic_policy_entries_n = sizeof(omnic_policy_entries) / sizeof(omnic_policy_entries[0]);
'''


def render(rows, src_note):
    body = []
    last = None
    for tag, ptag, row in rows:
        if tag != last:
            body.append('')
            body.append('    /* ---- %s ---- */' % tag)
            last = tag
        body.append('    ' + row)
    return HEADER % src_note + '\n'.join(body) + '\n' + FOOTER


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fit', required=True)
    ap.add_argument('--baseline', default='src/policy/policy_table.gen.c',
                    help='基表来源（未 fit 类的行保留源）')
    ap.add_argument('--out', default='src/policy/policy_table.gen.c')
    ap.add_argument('--check', action='store_true', help='只报告 diff，不写文件')
    args = ap.parse_args()

    fit = json.loads(Path(args.fit).read_text(encoding='utf-8'))
    fitted_classes = fit.get('classes', fit)
    baseline = parse_current(args.baseline)
    rows = build_rows(baseline, fitted_classes)
    src_note = ('由 %s 生成（%s），覆盖类: %s。'
                % (Path(args.fit).name, time.strftime('%Y-%m-%d'),
                   ', '.join(sorted(fitted_classes)) or '无'))
    text = render(rows, src_note)

    if args.check:
        import difflib
        cur = Path(args.out).read_text(encoding='utf-8')
        diff = [l for l in difflib.unified_diff(cur.splitlines(), text.splitlines(),
                                                'current', 'generated', lineterm='')
                if l.startswith(('+', '-')) and not l.startswith(('+++', '---'))]
        if not diff:
            print('gen --check: 表无变化')
            return
        print('gen --check: %d 行变化' % len(diff))
        for l in diff:
            print('  ' + l)
    else:
        Path(args.out).write_text(text, encoding='utf-8')
        print('gen: 写入 %s（32 行，覆盖类: %s）' % (args.out, ', '.join(sorted(fitted_classes)) or '无'))


if __name__ == '__main__':
    main()

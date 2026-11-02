"""corpora_gen: 合成语料生成器（M6 训练对齐修正）。

背景: M5 的 graphic 三件套（pattern_block/chart/gradient）为 25MB 真实风格图案，
4KiB 窗重复率实测≈0，γ 判据（dup4k>=0.30）下全部路由 PHOTO——GRAPHIC 类
（大色块合成图/扫描件/扁平 UI 导出）反而没有合法训练语料。
本生成器产出 d4k>=0.30 形态的三个分布（slides/scan_doc/ui_flat）。

用法:
  python tools/train/corpora_gen.py --out tools/train/corpora/graphic [--mib 24]
生成后用 feature_probe 走查 d4k（门禁 >=0.30）再入矩阵。
"""
import argparse
import os
import random
from pathlib import Path

WIDTH = 2900  # 24bpp: 行 8700B，4KiB 窗=1365px，跨行连续


def bmp_header(w, h, data_len):
    row = w * 3
    stride = (row + 3) & ~3
    size = stride * h + 54
    hdr = bytearray(54)
    hdr[0:2] = b'BM'
    hdr[2:6] = size.to_bytes(4, 'little')
    hdr[10:14] = (54).to_bytes(4, 'little')
    hdr[14:18] = (40).to_bytes(4, 'little')
    hdr[18:22] = w.to_bytes(4, 'little')
    hdr[22:26] = h.to_bytes(4, 'little')
    hdr[26:28] = (1).to_bytes(2, 'little')
    hdr[28:30] = (24).to_bytes(2, 'little')
    hdr[34:38] = data_len.to_bytes(4, 'little')
    return bytes(hdr)


def gen_slides(h, rng):
    """PPT 导出风格: 每张 350px 高，纯色背景 + 标题条 + 文字线段（灰）。"""
    out = bytearray()
    palette = [(45, 62, 80), (52, 120, 180), (70, 130, 70), (120, 60, 90),
               (180, 130, 50), (60, 60, 65), (30, 90, 110), (95, 75, 60)]
    i = 0
    while len(out) < h * WIDTH * 3:
        bg = palette[i % len(palette)]
        i += 1
        for y in range(350):
            if y < 50:  # 标题条
                row = bytes([max(0, bg[2] - 40), max(0, bg[1] - 40), max(0, bg[0] - 40)]) * WIDTH
            elif 90 < y < 300 and (y - 90) % 36 < 10:  # 文字线
                seg = bytes([210, 210, 215])
                x = 0
                row = bytearray()
                while x < WIDTH:
                    n = rng.randint(30, 260)
                    row += seg * n
                    x += n
                    n = rng.randint(20, 120)
                    row += bytes(bg) * n
                    x += n
                row = bytes(row)
            else:
                row = bytes(bg) * WIDTH
            out += row
    return bytes(out[:h * WIDTH * 3])


def gen_scan_doc(h, rng):
    """扫描文档风格: 白底 + 规则间隔的文字行（黑段+间隙）。"""
    white = bytes([255, 255, 255])
    out = bytearray()
    while len(out) < h * WIDTH * 3:
        for y in range(40):
            if 12 < y < 32 and rng.random() < 0.5:  # 一行文字
                row = bytearray()
                x = 0
                while x < WIDTH:
                    n = rng.randint(4, 18)
                    row += bytes([20, 20, 20]) * n
                    x += n
                    n = rng.randint(6, 24)
                    row += white * n
                    x += n
                row = bytes(row)
            else:
                row = white * WIDTH
            out += row
    return bytes(out[:h * WIDTH * 3])


def gen_ui_flat(h, rng):
    """扁平 UI 导出风格: 水平大色段（400px 高）+ 嵌入控件块（10% 面积）。"""
    out = bytearray()
    palette = [(70, 110, 200), (220, 80, 90), (250, 190, 60), (90, 170, 110),
               (130, 90, 180), (70, 75, 85), (200, 120, 50), (40, 150, 160)]
    i = 0
    while len(out) < h * WIDTH * 3:
        bg = palette[i % len(palette)]
        i += 1
        controls = [(rng.randint(100, 2600), rng.randint(30, 360),
                     rng.randint(60, 400), rng.randint(15, 40))
                    for _ in range(rng.randint(3, 8))]
        for y in range(400):
            row = bytearray(bytes(bg) * WIDTH)
            for cx, cy, cw, chh in controls:
                if cy <= y < cy + chh:
                    c = palette[(i + cx) % len(palette)]
                    row[cx * 3:(cx + cw) * 3] = bytes(c) * cw
            out += row
    return bytes(out[:h * WIDTH * 3])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--mib', type=int, default=24)
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    h = args.mib * 1024 * 1024 // (WIDTH * 3)
    rng = random.Random(20261102)
    for name, fn in [('slides', gen_slides), ('scan_doc', gen_scan_doc), ('ui_flat', gen_ui_flat)]:
        data = fn(h, rng)
        p = out / name / ('%s.bmp' % name)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(bmp_header(WIDTH, h, len(data)) + data)
        print('%s: %.1f MiB -> %s' % (name, len(data) / 1048576, p))


if __name__ == '__main__':
    main()

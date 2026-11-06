#!/usr/bin/env python3
"""聚合 gcov 行覆盖报告(库源码 src/**)。

用法:coverage.py <gcov 根目录> <仓库源码目录>
  gcov 文件由 `gcov -o <objdir> <objfile>` 生成于根目录(递归)。
输出按覆盖率升序的逐文件表 + 总体行覆盖(打印到 stdout)。
"""
import glob
import re
import sys

GCOV_ROOT = sys.argv[1] if len(sys.argv) > 1 else "."
SRC_DIR = (sys.argv[2] if len(sys.argv) > 2 else "").rstrip("/") + "/src/"

rows = {}
for g in glob.glob(GCOV_ROOT + "/**/*.gcov", recursive=True):
    src = None
    total = covered = 0
    with open(g, errors="ignore") as fh:
        for line in fh:
            if "0:Source:" in line and line.strip().startswith("-"):
                src = line.split("0:Source:")[1].strip()
                continue
            if src is None or not src.startswith(SRC_DIR):
                continue
            mark = line.split(":", 1)[0].strip()
            if mark == "-":          # 非可执行行
                continue
            total += 1
            if not mark.startswith("#"):   # ##### = 未执行
                covered += 1
    if src and total:
        f = src.split("/src/", 1)[1]
        p = covered * 100.0 / total
        if f not in rows or p > rows[f][0]:   # 多 target 取最高
            rows[f] = (p, total)

rows = sorted((p, n, f) for f, (p, n) in rows.items())
tc = sum(p * n for p, n, f in rows)
tl = sum(n for p, n, f in rows)
for p, n, f in rows:
    print(f"{p:6.2f}%  {n:4d}  {f}")
if tl:
    print(f"\nOVERALL: {tc/(tl*100)*100:.2f}%  ({tl} executable lines)")
else:
    print("no gcov data — run tests with --coverage first")

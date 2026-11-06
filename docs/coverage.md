# 测试覆盖率报告(库源码,gcov 实测)

> 生成方式:`cmake -B build-cov -DCMAKE_CXX_FLAGS=--coverage` → `ctest` → 对 `CMakeFiles/*.dir/**/*.gcov` 统计。刷新日期:2026-11-06,总体 **89.98%**(1787 可执行行)。

```
 68.85%    61  src/core/failure.cpp
 74.74%    95  src/core/pack.cpp
 75.31%    81  src/nodes/checkpoint.cpp
 80.39%   102  src/core/journal_file.cpp
 86.79%    53  src/core/schema.cpp
 86.84%    38  src/nodes/for_each.cpp
 89.39%    66  src/nodes/reactive_guard.cpp
 89.47%    95  src/core/task.cpp
 91.67%    36  src/nodes/compensable.cpp
 92.31%    39  src/nodes/while_loop.cpp
 92.50%    40  src/nodes/switch_n.cpp
 92.97%   128  src/core/bridge_node.cpp
 93.28%   253  src/core/engine_core.cpp
 94.12%    17  src/core/nebula_ctx.cpp
 94.46%   343  src/core/engine.cpp
 94.48%   145  src/core/scheduler.cpp
 95.83%    72  src/core/tree_check.cpp
 98.28%    58  src/core/node_stats.cpp
100.00%     3  src/core/engine_core.hpp
100.00%    16  src/core/journal.cpp
100.00%    46  src/nodes/retry_with_delay.cpp

OVERALL: 89.98%  (1787 executable lines)
```

低覆盖项即后续补测的优先目标:failure.cpp(失败码表分支)、pack.cpp(清单错误分支)、checkpoint.cpp(恢复边界)、journal_file.cpp(损坏输入分支)。

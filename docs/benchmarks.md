# 性能基准

`benchmarks/bench_core.cpp` 的实测基线。复现:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/benchmarks/nebulabt_bench
```

## 基线(2026-11-06)

环境:Intel Core Ultra 5 225,g++ 11.4 -O3(Release),Linux 6.8,
BT.CPP 4.9.0(ROS humble 包)。单线程稳态,数字仅供相对比较与回归对照。

| 指标 | 数值 | 说明 |
|---|---|---|
| B1 树创建+校验(15 节点) | ~42 µs/次 | createTreeFromText + L2 数据流检查;包加载期门禁的成本量级 |
| B2 任务周转(15 节点) | ~150 µs/任务 | submit → 终态全链路(含创建),≈ 6.7k 任务/s/引擎 |
| B3 树内 tick(2 节点循环) | ~630 ns/tick | 事件驱动的单次 tick 成本下限(同步节点) |
| B4 journal 帧落盘 | ~2.5 µs/事件(≈ 59 MB/s) | 长度前缀 + CRC32 + fflush |
| B5 explain(终态,15 节点) | ~23 µs/次 | 终态直接树遍历 |
| 峰值 RSS | ~45 MB | 整个基准进程(含 BT.CPP 与 2000+ 任务运行) |

## 解读(嵌入式视角)

- **tick 成本 ~630ns**:即使 1kHz 全树重 tick(反应式守护巡检的极端情况),
  单引擎 CPU 占用 < 0.1%,轮询式业务不存在。
- **任务周转 ~150µs**:大头是树创建(B1 ~42µs)+ JSON/slots 注入 +
  事件跨线程往返;对机器人任务(秒~分钟级)完全可忽略。
- **journal ~2.5µs/事件**:PerEvent fsync 关闭时落盘不构成瓶颈;
  开启 fsync 后受 flash 写入速度支配(flash 磨损 vs 安全的权衡见
  `JournalFsync`)。

## 回归对照

基准数字会随机器浮动;CI 不做性能门禁(避免抖动误报),
但 `nebulabt_bench` 应保持可运行,发布前跑一次对照本表。

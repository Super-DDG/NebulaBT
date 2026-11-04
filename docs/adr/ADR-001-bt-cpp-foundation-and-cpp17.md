# ADR-001: 基于 BehaviorTree.CPP 4.9 与 C++17,不重写树引擎

- 状态:已接受
- 日期:2026-11-04

## 背景

NebulaBT 的目标是面向业务开发的行为树编排框架。一个根本性选择摆在面前:
从零实现树引擎,还是在现有引擎之上构建应用层?

## 决策

1. **以 BehaviorTree.CPP v4.9 为树引擎,通过包依赖使用,不修改其源码。**
2. **语言标准定为 C++17**,不使用 C++20。

## 理由

**不重写引擎**:BT.CPP 已经解决了最难且最通用的部分——XML 解析、typed ports
与加载期静态校验、blackboard 与 remapping、控制节点族、subtree 参数化、
Groot2 可视化协议。重写等于在维护者( davidefaconti )的主场与其竞争,
且会永久背上"追赶特性"的负担。BT.CPP 4.x 还官方支持运行期传入 `PortsList`
注册节点(`registerNodeType(id, ports, args...)`),这正是 NebulaBT
"端口来自 action 实例"的桥接所需,无需任何 hack。

**C++17 而非 C++20**:

- BT.CPP 4.x 自身要求 C++17,跟随宿主库是唯一自洽的选择;
- 覆盖面:ROS2 默认工具链、主机游戏工作室的老编译器、嵌入式机器人平台;
- C++20 协程在 MSVC 之外的实现质量仍然参差,游戏团队常禁用。

**代价**:协程不可用,节点 API 采用回调续传设计(见 ADR-002)。

## 后果

- 正面:树语义的正确性由最成熟的开源实现背书;Groot2 生态免费获得;
  维护面收敛到应用层。
- 负面:受制于 BT.CPP 的 API 演进节奏;其 `SyncActionNode` 禁止 RUNNING 且
  `halt()` 为 final,异步桥必须基于 `StatefulActionNode`;
  blackboard 无法序列化"正在 RUNNING 的节点"(持久化设计因此走
  journal + checkpoint 路线,见 DESIGN 4.7)。

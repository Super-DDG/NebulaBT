# ADR-003: 统一 JSON 实现(BT.CPP 内置)

- 状态:已接受
- 日期:2026-11-04

## 背景

框架多处使用 JSON(失败记录、explain 报告、slots、done 的 result)。
系统 nlohmann/json 与 BT.CPP 内置 vendored 版本
(`behaviortree_cpp/contrib/json.hpp`)可能同时存在于用户工程中。

## 决策

NebulaBT 全部使用 BT.CPP 内置的 nlohmann::json,不引入独立 JSON 依赖。

## 理由

- 前车之鉴:某机器人项目中,BT.CPP vendor json 与系统 json 跨翻译单元
  ABI 不一致,被迫在公共 API 层手工规避(拼接字符串、禁止暴露 json 类型)
- 统一来源后,`nlohmann::json` 在 NebulaBT 公共 API 中可以安全出现,
  用户从 BT.CPP 侧传入的 json 类型天然兼容
- 少一个依赖,"5 分钟跑通"目标(见 DESIGN)更容易达成

## 后果

- 用户工程若使用另一版本的 nlohmann/json,在边界处需要自行转换
  (通常是字符串往返,与上述机器人项目的做法一致)
- 跟随 BT.CPP 的 json 版本演进,不自行升级

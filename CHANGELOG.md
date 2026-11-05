# Changelog

All notable changes to NebulaBT are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/); versioning is [SemVer](https://semver.org/).

## [1.0.0] - 2026-11-05

First tagged release. Core feature set complete and covered by 21 test cases
(2 binaries, incl. a real `SIGKILL` recovery test).

### Added

- **Execution model**: event-driven scheduler (wake on completion callbacks /
  timers / external commands; zero polling); callback-continuation node SDK
  (`NebulaAction` / `NebulaCtx` / `Done`), C++17, built on BehaviorTree.CPP 4.9.
- **Tasks**: lifecycle (submit / wait / cancel / onFinished), typed blackboard
  slots injection, task-level timeout policy (`TASK_TIMEOUT`).
- **Failure as data**: structured `FailureRecord`s, auto-capture of silent
  node failures (`NODE_FAILURE`), `explain()` with per-node
  attempts / running_ms / first_ms / last_ms via full-tree status tracing.
- **Dynamic tree primitives**: `ForEach`, `WhileLoop`, `SwitchN` (unbounded
  cases), `RetryWithDelay` (inter-attempt delay + wall-clock budget).
- **Journal & recovery**: per-task event journal (sink for persistence),
  `Checkpoint` decorator with full-blackboard snapshots, resume via
  `TaskSpec.resume_journal` — completed checkpoints skip their subtree and
  restore state; committed side effects never re-execute. Verified by a
  fork + SIGKILL test.
- **Tree packs**: directory-based versioned assets (`pack.json` + trees);
  load-time validation of every tree (parse + schema dataflow); templates
  with slot-defaults (variants without forks); `requires` dependency checks;
  canary routing by deterministic task-id hash with explicit version pinning.
- **Testing**: `testkit::ScriptedAction` mock with input recording.
- **CLI**: `nebula validate` (CI gate), `nebula journal`, `nebula explain`,
  `nebula doc` (tree → mermaid flowchart).

## [0.1.0] - 2026-11-04

Initial skeleton: scheduler, SDK, bridge, ForEach, failure records, explain,
task lifecycle.

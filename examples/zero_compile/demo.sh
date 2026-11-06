#!/usr/bin/env bash
# ============================================================================
# 0 编译新增任务演示(CTest 用)
#
#   1. 宿主启动,加载 packs/(JSON 巡逻包;YAML 押送包在 YAML 支持启用时)
#   2. 提交已有任务:巡逻成功;押送(如已加载)展示补偿+守护
#   3. 运行中把 newpack_delivery 投进 packs/ → load → 提交新任务类型
#      —— 不改代码、不重编译,新任务端到端跑通(失败路径:重试耗尽 +
#      自动补偿,展示可观测性)
# ============================================================================
set -u

HOST="$1"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d /tmp/nebulabt_zero_compile.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

cp -r "$SRC_DIR/packs/." "$WORK/packs/"
mkdir -p "$WORK/packs"

OUT="$WORK/out.txt"

# ---- 1+2:启动宿主,提交已有任务 ----
{
  echo '{"task_type": "PATROL_AREA", "slots": {"zone": "lobby", "waypoints": ["gate", "plaza", "tower"]}}'
  echo quit
} | "$HOST" "$WORK/packs" > "$OUT" 2>&1

grep -q "PATROL_AREA -> SUCCEEDED" "$OUT" || { echo "patrol failed"; cat "$OUT"; exit 1; }
grep -q "checkpoint" "$OUT" || { echo "no checkpoints in journal"; exit 1; }
# YAML 支持是否启用以"包实际加载"为准(pack.yaml 文件在默认构建下也存在,
# 而 [load] FAILED 行也会含目录名,故匹配加载清单行)
if grep -q "\[type\] ESCORT_GUEST" "$OUT"; then
  {
    echo '{"task_type": "ESCORT_GUEST", "slots": {"guest_name": "alice", "guest_present": true}}'
    echo quit
  } | "$HOST" "$WORK/packs" >> "$OUT" 2>&1
  grep -q "ESCORT_GUEST -> SUCCEEDED" "$OUT" || { echo "escort failed"; cat "$OUT"; exit 1; }
  grep -q "guest accompanied" "$OUT" || { echo "guard held path missing"; exit 1; }
else
  echo "(yaml pack not loaded - NEBULABT_WITH_YAML off, skipping yaml assertions)" >> "$OUT"
fi

# ---- 3:运行中新增任务类型(0 编译的核心演示)----
{
  echo "load $SRC_DIR/newpack_delivery"
  echo '{"task_type": "DELIVER_PARCEL", "slots": {"parcel": "box-42", "destination": "room-301"}}'
  echo quit
} | "$HOST" "$WORK/packs" >> "$OUT" 2>&1

grep -q "\[load\] ok" "$OUT" || { echo "runtime load failed"; cat "$OUT"; exit 1; }
grep -q "DELIVER_PARCEL -> FAILED" "$OUT" || { echo "new task did not run"; cat "$OUT"; exit 1; }
grep -q "PICKUP_BUSY" "$OUT" || { echo "failure code missing"; exit 1; }
grep -q "courier released" "$OUT" || { echo "compensation did not run"; exit 1; }

echo "zero-compile demo PASSED"

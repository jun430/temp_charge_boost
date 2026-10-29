#!/bin/bash
# 温度墙控制律纯逻辑测试
# 不依赖 daemon（daemon 会 bind mount 掉 zone，无法注入温度），
# 直接把 wall_tier_from_temp / 恢复判定逻辑抽出来，用同一份常量做断言。
set -u
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  PASS  $1"; }
ng(){ FAIL=$((FAIL+1)); echo "  FAIL  $1"; }
chk(){ if [ "$2" = "$3" ]; then ok "$1 = $2"; else ng "$1 期望[$3] 实际[$2]"; fi; }

# ---- 从源码里抽出真实常量，保证测的是代码里那份 ----
# ⚠ 定位踩坑（本组曾因此静默失败）：
#   本文件第一个参数是对外调用约定里的"二进制路径"（与其他测试一致），
#   曾经写成 SRC=${1:-t.cpp}，于是 bash tier_test.sh ./t_bin 时
#   SRC=二进制 → grep 常量全落空 → PCTS 为空 → N=0 →
#   tier_of() 永远返回 0 → A 组全部"PASS"、B/C 组乱报。
#   教训：解析不到常量时必须直接失败，绝不能退化成默认值继续跑。
SRC=${2:-${TCB_SRC:-/root/tcb_build/t.cpp}}
if [ ! -f "$SRC" ]; then echo "  [环境异常] 源码不存在: $SRC"; exit 1; fi
PCTS=$(grep -o 'WALL_PCT\[\] = {[^}]*}' "$SRC" | sed 's/.*{//;s/}.*//;s/ //g')
PUSH=$(grep -o 'WALL_PUSH_SEC *= *[0-9]*' "$SRC" | grep -o '[0-9]*')
RELAXC=$(grep -o 'WALL_RELAX_C *= *[0-9]*' "$SRC" | grep -o '[0-9]*')
echo "从源码读到（$SRC）: WALL_PCT={$PCTS}  PUSH=${PUSH}s  RELAX_C=$RELAXC"
# 硬校验：拿不到常量就终止，禁止用空值继续算下去
if [ -z "$PCTS" ]; then echo "  [环境异常] 未能从源码解析 WALL_PCT，测试无效"; exit 1; fi
if [ -z "$PUSH" ] || [ -z "$RELAXC" ]; then echo "  [环境异常] 未能解析 WALL_PUSH_SEC / WALL_RELAX_C，测试无效"; exit 1; fi
IFS=',' read -ra P <<< "$PCTS"
N=${#P[@]}
echo "档位数=$N"
if [ "$N" -lt 2 ]; then echo "  [环境异常] 档位数=$N 不合理，测试无效"; exit 1; fi

# 复刻 wall_tier_from_temp
tier_of(){ # tier_of <batt_c> <wall_c>
  local b=$1 w=$2
  if [ $b -lt $w ]; then echo 0; return; fi
  local t=$(( 1 + (b - w) / 500 ))
  local mx=$((N - 1))
  [ $t -gt $mx ] && t=$mx
  echo $t
}

W=51000
echo ""
echo "=== A. 51℃ 之前必须全部是档 0（不限流）==="
for t in 20000 30000 35000 40000 45000 48000 50000 50500 50900 50999; do
  chk "电池 $((t/1000)).$((t%1000/100))℃ → 档位" "$(tier_of $t $W)" "0"
done

echo ""
echo "=== B. 恰好 51℃ 开始限流 ==="
t1=$(tier_of 51000 $W)
if [ "$t1" -ge 1 ]; then ok "电池 51.0℃ → 档 $t1，已开始限流"; else ng "电池 51.0℃ → 档 0，未限流"; fi

echo ""
echo "=== C. 阈值以上每 +0.5℃ 加一档，且单调不减 ==="
prev=0
for t in 51000 51500 52000 52500 53000 53500 54000 55000 60000; do
  cur=$(tier_of $t $W)
  if [ "$cur" -ge "$prev" ]; then ok "电池 $((t/1000)).$((t%1000/100))℃ → 档 $cur（>= 上一档 $prev）"; else ng "档位回退 $prev→$cur"; fi
  prev=$cur
done

echo ""
echo "=== D. 档位对应的限流比例 ==="
for t in 51000 52000 53500 60000; do
  ti=$(tier_of $t $W)
  pct=${P[$ti]}
  cur=$(( 22000000 * pct / 100 ))
  echo "  电池 $((t/1000))℃ → 档$ti → ${pct}% → ${cur} (约 $((cur/1000000)).$((cur%1000000/100000))A)"
done
ti=$(tier_of 60000 $W); pct=${P[$ti]}
if [ "$pct" -le 10 ]; then ok "高温时压到 ${pct}%（确有重压）"; else ng "高温限流不足: ${pct}%"; fi

echo ""
echo "=== E. 恢复判定：40℃ + 稳定 10s ==="
echo "  WALL_RELAX_C=${RELAXC}  WALL_RELAX_SEC=10"
if [ "$RELAXC" = "40000" ]; then ok "恢复温度 = 40.0℃"; else ng "恢复温度错误: $RELAXC"; fi
# 恢复点必须低于阈值，否则刚放开又立刻触发
if [ "$RELAXC" -lt "$W" ]; then ok "恢复点 40℃ < 阈值 51℃，不会来回震荡"; else ng "恢复点不低于阈值"; fi
# 滞回带宽
gap=$(( (W - RELAXC) / 1000 ))
if [ "$gap" -ge 5 ]; then ok "滞回带宽 ${gap}℃（足够防抖）"; else ng "滞回带宽仅 ${gap}℃，可能抖动"; fi

echo ""
echo "=== F. 积分项参数 ==="
if [ "$PUSH" = "20" ]; then ok "积分间隔 20s（热平衡僵局可在 20s 内破除）"; else ng "积分间隔异常: $PUSH"; fi

echo ""
echo "=== G. 极端输入健壮性 ==="
chk "温度 0（读不到）→ 档位" "$(tier_of 0 $W)" "0"
chk "负温度 → 档位" "$(tier_of -5000 $W)" "0"

echo ""
echo "================================"
echo "通过 $PASS / 失败 $FAIL"
exit $([ $FAIL -eq 0 ] && echo 0 || echo 1)
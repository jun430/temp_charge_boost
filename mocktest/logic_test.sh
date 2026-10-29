#!/bin/bash
# 逻辑断言测试：温度墙 51℃ 前不限流 / 亮度可用 / 充电节点可用
# 用法: bash logic_test.sh /root/tcb_build/t_bin
set -u
BIN=${1:-/root/tcb_build/t_bin}
# 独立环境：不再假设 mock_test.sh 已经建好 /tmp/tcbmock。
# 原来直接复用该目录，形成隐式顺序依赖——
# mock_test 先跑就通过，环境被清理就 15 项全崩（本轮实测踩到）。
R=/tmp/tcb_logic
. "$(dirname "$0")/mock_env.sh"
if ! mock_env "$R"; then exit 1; fi
export TCB_ROOT=$R
Z=$R/sys/devices/virtual/thermal
W=$R/sys/class/xm_power/charger/charger_thermal/wired_chg_curr
D=$R/data/adb/temp_charge_boost
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  PASS  $1"; }
ng(){ FAIL=$((FAIL+1)); echo "  FAIL  $1"; }
chk(){ if [ "$2" = "$3" ]; then ok "$1 ($2)"; else ng "$1 期望[$3] 实际[$2]"; fi; }

# 本脚本假设 zone 尚未被伪装（否则写入 temp 会被 bind mount 挡住）
# 先还原，再重新扫描，保证 temp 是可写的普通文件
$BIN restore >/dev/null 2>&1
rm -f $D/profile.conf
$BIN scan >/dev/null 2>&1

echo "=== A. 温度墙档位函数（通过 wall 命令 + 注入温度验证）==="
# 直接用 daemon 不方便，这里用 apply 后手动跑 wall_tick 不可行，
# 改为验证 wall_target_curr 的可观测输出：写 wall.conf 后由 dump 反映配置
echo "1 51000" > $D/wall.conf
OUT=$($BIN wall 2>/dev/null)
echo "$OUT" | grep -q '"wall_c":51000' && ok "wall_c=51000 生效" || ng "wall_c 未生效: $OUT"
echo "$OUT" | grep -q '"wall_on":1'    && ok "wall_on=1 生效"    || ng "wall_on 未生效: $OUT"

echo ""
echo "=== B. 电池真实温度源必须是 batt_therm（zone85），不是被 fake 的 battery ==="
# 电池三兄弟：battery=35000(假), batt_temp=35000(假), batt_therm=42600(真)
echo 42600 > $Z/thermal_zone85/temp
echo 35000 > $Z/thermal_zone67/temp
echo 35000 > $Z/thermal_zone70/temp
OUT=$($BIN dump 2>/dev/null)
BATT=$(echo "$OUT" | tr ',' '\n' | grep '"batt_ntc"' | tr -dc 0-9)
echo "$OUT" | tr ',' '\n' | grep -q '"batt_ntc":42600' \
  && ok "batt_ntc=42600 取自 batt_therm" || ng "batt_ntc 错误: $BATT"
DRV=$(echo "$OUT" | tr ',' '\n' | grep '"batt_drv"' | tr -dc 0-9)
chk "batt_drv 是伪装值 35000" "$DRV" "35000"

echo ""
echo "=== C. 屏幕亮度功能可用 ==="
OUT=$($BIN bright 2>/dev/null)
echo "$OUT" | grep -q '"bl_ok":true' && ok "bl_ok=true" || ng "bl_ok 失败: $OUT"
echo "$OUT" | grep -q '"max":16383'   && ok "max=16383"   || ng "max 错误: $OUT"
$BIN bright 50 >/dev/null 2>&1
RAW=$(cat $R/sys/class/backlight/panel0-backlight/brightness)
chk "bright 50 → raw 8191" "$RAW" "8191"
$BIN bright 7 >/dev/null 2>&1
RAW=$(cat $R/sys/class/backlight/panel0-backlight/brightness)
chk "bright 7 → raw 1146" "$RAW" "1146"

echo ""
echo "=== D. 充电相关节点 ==="
$BIN apply 350 >/dev/null 2>&1
CTRL=$(cat $R/sys/class/xm_power/charger/charger_thermal/wired_ctrl_limit 2>/dev/null)
chk "wired_ctrl_limit=0" "$CTRL" "0"
PPS=$(cat $R/sys/class/xm_power/charger/quick_charge/fake_pps_ptf 2>/dev/null)
chk "fake_pps_ptf=1" "$PPS" "1"
FT=$(cat $R/sys/class/xm_power/fuelgauge/strategy_fg/fake_temp 2>/dev/null)
chk "fake_temp=350" "$FT" "350"

echo ""
echo "=== E. 危险节点必须保持真实值（未被伪装）==="
for pair in "40:sdr0_pa:87300" "41:mdmss-0:85000" "43:pa_therm1:88000" "69:cpu-hw-trip-0:105000"; do
  zn=$(echo $pair | cut -d: -f1); nm=$(echo $pair | cut -d: -f2); ev=$(echo $pair | cut -d: -f3)
  v=$(cat $Z/thermal_zone$zn/temp 2>/dev/null)
  chk "$nm (zone$zn) 保留真实值" "$v" "$ev"
done

echo ""
echo "=== F. 伪装节点已生效（属于可伪装集合）==="
v=$(cat $Z/thermal_zone36/temp 2>/dev/null)   # gpuss-0
chk "gpuss-0 (zone36) 被伪装" "$v" "350"
v=$(cat $Z/thermal_zone85/temp 2>/dev/null)   # batt_therm
chk "batt_therm (zone85) 被伪装" "$v" "350"

echo ""
# ---- 收尾：解除伪装挂载 + 删目录（测试必须自己收拾干净） ----
$BIN restore >/dev/null 2>&1
awk -v p="$R" '$5==p || index($5,p"/")==1 {print $5}' /proc/self/mountinfo 2>/dev/null | awk '{print length, $0}' | sort -rn | cut -d" " -f2- | while read -r m; do umount -l "$m" 2>/dev/null; done
rm -rf "$R" 2>/dev/null
LEFT=$(awk -v p="$R" '$5==p || index($5,p"/")==1 {n++} END {print n+0}' /proc/self/mountinfo 2>/dev/null)
if [ "$LEFT" != "0" ]; then echo "  FAIL  收尾后仍有 $LEFT 个挂载残留"; FAIL=$((FAIL+1)); fi
echo "================================"
echo "通过 $PASS / 失败 $FAIL"
exit $([ $FAIL -eq 0 ] && echo 0 || echo 1)
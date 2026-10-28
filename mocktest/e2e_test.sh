#!/bin/bash
# 端到端闭环测试（真实 mount，用 unshare 隔离，不影响宿主）
# 关键：daemon 会 bind mount 掉 zone 的 temp，所以注入温度必须在
#       daemon 眼里"真实"——通过 REAL_DIR 快照不可行（那是只读产物），
#       因此这里换思路：直接验证 daemon 从"未被挂载的另一个 zone"读到温度。
#
# 更简单可靠的做法：用 wall 命令 + 手动构造 REAL_DIR 新鲜快照，
# 让 dump/wall 独立进程路径（②级回退）读到我们想要的值。
set -u
BIN=${1:-/root/tcb_build/t_bin}
R=/tmp/tcbe2e
rm -rf $R; mkdir -p $R
export TCB_ROOT=$R
Z=$R/sys/devices/virtual/thermal
W=$R/sys/class/xm_power/charger/charger_thermal/wired_chg_curr
D=$R/data/adb/temp_charge_boost
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  PASS  $1"; }
ng(){ FAIL=$((FAIL+1)); echo "  FAIL  $1"; }

mkz(){ mkdir -p "$Z/thermal_zone$1"; echo -n "$2" > "$Z/thermal_zone$1/type"; echo "$3" > "$Z/thermal_zone$1/temp"; }
mkz 67 "battery" 35000
mkz 85 "batt_therm" 42000
mkz 36 "gpuss-0" 55000
X=$R/sys/class/xm_power
mkdir -p $X/fuelgauge/strategy_fg $X/charger/charger_thermal $X/charger/quick_charge
echo 350 > $X/fuelgauge/strategy_fg/fake_temp
echo 0 > $X/charger/charger_thermal/wired_ctrl_limit
echo 1 > $X/charger/quick_charge/fake_pps_ptf
echo 22000000 > $W
P=$R/sys/class/power_supply
mkdir -p $P/battery $P/usb
echo 350 > $P/battery/temp; echo Charging > $P/battery/status; echo 1 > $P/usb/online

echo "=== 1. 首次运行自动扫描（profile 不存在）==="
rm -rf $D
$BIN apply 350 >/dev/null 2>&1
[ -f $D/profile.conf ] && ok "profile.conf 自动生成（安装即适配）" || ng "未自动生成 profile"
grep -q 'batt_type=batt_therm' $D/profile.conf && ok "自动识别电池真实源=batt_therm" || ng "电池源识别错误"
grep -q 'cur_ok=1' $D/profile.conf && ok "识别到充电控制点可写" || ng "充电控制点未识别"

echo ""
echo "=== 2. 伪装已建立，battery 被改、batt_therm 也被改 ==="
v=$(cat $Z/thermal_zone67/temp); [ "$v" = "350" ] && ok "battery 被伪装为 350" || ng "battery=$v"
v=$(cat $Z/thermal_zone85/temp); [ "$v" = "350" ] && ok "batt_therm 被伪装为 350" || ng "batt_therm=$v"

echo ""
echo "=== 3. 恢复到真实值 ==="
$BIN restore >/dev/null 2>&1
v=$(cat $Z/thermal_zone85/temp); [ "$v" = "42000" ] && ok "restore 后 batt_therm 回到真实 42000" || ng "restore 失败: $v"

echo ""
echo "=== 4. 壁纸：wall 命令三态 ==="
echo "1 51000" > $D/wall.conf 2>/dev/null || { mkdir -p $D; echo "1 51000" > $D/wall.conf; }
O=$($BIN wall 2>/dev/null)
echo "$O" | grep -q '"wall_on":1' && ok "wall 开启状态可读" || ng "wall 读取失败: $O"
$BIN wall 52 >/dev/null 2>&1
grep -q "52" $D/wall.conf && ok "wall 52 阈值写入成功" || ng "阈值写入失败: $(cat $D/wall.conf)"
$BIN wall off >/dev/null 2>&1
grep -q "^0" $D/wall.conf && ok "wall off 关闭成功" || ng "关闭失败: $(cat $D/wall.conf)"

echo ""
echo "=== 5. 亮度上下文（不碰 daemon 自动调节）==="
B=$R/sys/class/backlight/panel0-backlight
mkdir -p $B; echo 16383 > $B/max_brightness; echo 1079 > $B/brightness
$BIN bright 30 >/dev/null 2>&1
v=$(cat $B/brightness)
[ "$v" = "4914" ] && ok "bright 30 → 4914（16383*30%）" || ng "bright 30 得到 $v"

echo ""
echo "=== 6. 日志包含可排查信息 ==="
for kw in "\[scan\]" "\[apply\]" "应用温度伪装" "恢复温度伪装"; do
  grep -q "$kw" $D/run.log 2>/dev/null && ok "日志含 [$kw]" || ng "日志缺 [$kw]"
done
echo "--- 日志节选 ---"
tail -12 $D/run.log 2>/dev/null

echo ""
echo "================================"
echo "通过 $PASS / 失败 $FAIL"
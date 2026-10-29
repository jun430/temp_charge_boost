#!/bin/bash
# 换机型适配测试：用完全不同的 zone 编号与命名，验证「不依赖硬编码编号」
set -u
BIN=${1:-/root/tcb_build/t_bin}
R=/tmp/tcbalt
# 先剥掉上一轮的残留挂载：bind mount 之下的目录 rm -rf 删不掉，
# 会在脏目录上叠加，导致测试结果不可信（本轮已实测踩到）。
if [ -d "$R" ]; then
  awk -v p="$R" '$5==p || index($5,p"/")==1 {print $5}' /proc/self/mountinfo 2>/dev/null \
    | awk '{print length, $0}' | sort -rn | cut -d' ' -f2- \
    | while read -r m; do umount -l "$m" 2>/dev/null; done
fi
rm -rf $R 2>/dev/null
if [ -e "$R" ]; then echo "  [环境异常] $R 无法清空，测试结果不可信"; exit 1; fi
mkdir -p $R
export TCB_ROOT=$R
Z=$R/sys/devices/virtual/thermal
W=$R/sys/class/xm_power/charger/charger_thermal/wired_chg_curr
D=$R/data/adb/temp_charge_boost
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  PASS  $1"; }
ng(){ FAIL=$((FAIL+1)); echo "  FAIL  $1"; }
mkz(){ mkdir -p "$Z/thermal_zone$1"; echo -n "$2" > "$Z/thermal_zone$1/type"; echo "$3" > "$Z/thermal_zone$1/temp"; }

# 故意用"错位"的编号：所有 zone 号整体 +200，且顺序打乱
mkz 213 "gpuss-0" 51000
mkz 200 "cpu-0-0-0" 43000
mkz 201 "cpu-0-0-1" 43200
mkz 260 "batt_therm" 41200
mkz 251 "battery" 35000
mkz 222 "conn_therm" 37500
mkz 240 "mdmss-0" 82000
mkz 241 "pa_therm1" 85000
mkz 255 "cpu-hw-trip-0" 105000

X=$R/sys/class/xm_power
mkdir -p $X/fuelgauge/strategy_fg $X/charger/charger_thermal $X/charger/quick_charge
echo 350 > $X/fuelgauge/strategy_fg/fake_temp
echo 0 > $X/charger/charger_thermal/wired_ctrl_limit
echo 1 > $X/charger/quick_charge/fake_pps_ptf
echo 22000000 > $W
P=$R/sys/class/power_supply
mkdir -p $P/battery $P/usb
echo 350 > $P/battery/temp; echo Charging > $P/battery/status; echo 1 > $P/usb/online
B=$R/sys/class/backlight/p1; mkdir -p $B; echo 2047 > $B/max_brightness; echo 1024 > $B/brightness

echo "=== 换机型：zone 编号全部移位、命名不同 ==="
$BIN scan >/dev/null 2>&1
P1=$($BIN profile 2>/dev/null)
echo "$P1"
echo ""
echo "$P1" | grep -q '电池真实温度源      = zone260' && ok "按 type 找到 batt_therm 在 zone260（非硬编码 85）" || ng "batt_therm 定位错误"
echo "$P1" | grep -q 'CPU 首传感器        = zone200' && ok "按 type 找到 CPU 在 zone200" || ng "CPU 定位错误"
echo "$P1" | grep -q 'conn_therm' && ok "找到充电口 conn_therm" || ng "充电口未找到"
echo "$P1" | grep -q 'max=2047' && ok "识别背光面板 max=2047（不同机型上限）" || ng "背光识别错误"

echo ""
echo "=== 危险节点在新编号下依然被排除 ==="
$BIN apply 350 >/dev/null 2>&1
v=$(cat $Z/thermal_zone240/temp); [ "$v" = "82000" ] && ok "mdmss-0(zone240) 保留真实值" || ng "mdmss-0 被伪装: $v"
v=$(cat $Z/thermal_zone241/temp); [ "$v" = "85000" ] && ok "pa_therm1(zone241) 保留真实值" || ng "pa_therm1 被伪装: $v"
v=$(cat $Z/thermal_zone255/temp); [ "$v" = "105000" ] && ok "cpu-hw-trip-0(zone255) 保留真实值" || ng "hw-trip 被伪装: $v"

echo ""
echo "=== 可伪装节点正常生效 ==="
v=$(cat $Z/thermal_zone200/temp); [ "$v" = "350" ] && ok "cpu-0-0-0(zone200) 被伪装" || ng "cpu 未被伪装: $v"
v=$(cat $Z/thermal_zone260/temp); [ "$v" = "350" ] && ok "batt_therm(zone260) 被伪装" || ng "batt_therm 未被伪装: $v"

echo ""
echo "=== 亮度按新面板上限换算 ==="
$BIN bright 50 >/dev/null 2>&1
v=$(cat $B/brightness); [ "$v" = "1023" ] && ok "bright 50 → 1023（2047*50%）" || ng "换算错误: $v"

echo ""
echo "=== 挂载失败必须体现在日志（本次应有明确成功记录）==="
grep -qE '挂载 [0-9]+ 个, 跳过 [0-9]+ 个, 失败 [0-9]+ 个' $D/run.log && ok "日志记录了挂载结果明细（挂载/跳过/失败）" || ng "日志缺挂载明细"
grep -q '失败 0 个' $D/run.log && ok "本次挂载无失败" || ng "存在挂载失败"
grep -c '挂载失败' $D/run.log >/dev/null 2>&1 && ok "失败计数通道存在" || true

echo ""
echo "================================"
# ---- 收尾：解除伪装挂载 + 删目录（测试必须自己收拾干净） ----
$BIN restore >/dev/null 2>&1
awk -v p="$R" '$5==p || index($5,p"/")==1 {print $5}' /proc/self/mountinfo 2>/dev/null | awk '{print length, $0}' | sort -rn | cut -d" " -f2- | while read -r m; do umount -l "$m" 2>/dev/null; done
rm -rf "$R" 2>/dev/null
LEFT=$(awk -v p="$R" '$5==p || index($5,p"/")==1 {n++} END {print n+0}' /proc/self/mountinfo 2>/dev/null)
if [ "$LEFT" != "0" ]; then echo "  FAIL  收尾后仍有 $LEFT 个挂载残留"; FAIL=$((FAIL+1)); fi
echo "通过 $PASS / 失败 $FAIL"
exit $([ $FAIL -eq 0 ] && echo 0 || echo 1)

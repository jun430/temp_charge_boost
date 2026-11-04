#!/bin/bash
# 用途：提交前冒烟——在完整 mock 环境（含 thermal_zone）验证语义探测的读/写/降级
# 依赖：/tmp/finalb/tcb
# 说明：do_scan 的输出走 logf（日志文件），不在 stdout，所以结果从 run.log 读
set +e

R=/tmp/sm
B=/tmp/finalb/tcb
W=${R}w
rm -rf "$R" "$W"
mkdir -p "$R/sys/class/thermal" "$R/sys/devices/virtual/thermal"

# thermal_zone0..2：一个电池、一个充电器、一个无关
i=0
for t in battery charger_thermal cpu; do
  z="$R/sys/devices/virtual/thermal/thermal_zone$i"
  mkdir -p "$z"
  echo "$t" > "$z/type"
  echo 35000 > "$z/temp"
  ln -sfn "$z" "$R/sys/class/thermal/thermal_zone$i"
  i=$((i+1))
done

# 充电节点
D="$R/sys/class/xm_power/charger/charger_thermal"
mkdir -p "$D"
for n in wired_chg_curr wired_ctrl_limit wired_thermal_remove; do echo 1 > "$D/$n"; done
mkdir -p "$R/sys/class/xm_power/charger/quick_charge"
echo 0 > "$R/sys/class/xm_power/charger/quick_charge/fake_pps_ptf"
mkdir -p "$R/sys/class/xm_power/fuelgauge/strategy_fg"
echo 300 > "$R/sys/class/xm_power/fuelgauge/strategy_fg/fake_temp"
mkdir -p "$R/sys/class/power_supply/battery" "$R/sys/class/power_supply/usb"
echo 30000 > "$R/sys/class/power_supply/battery/temp"
echo Charging > "$R/sys/class/power_supply/battery/status"
echo 1 > "$R/sys/class/power_supply/usb/online"

export TCB_ROOT=$R
export TCB_WORK=$W
export TCB_ZONE_ROOT=$R/sys/devices/virtual/thermal
export TCB_ZONE_CLASS=$R/sys/class/thermal

echo "=== S1 原状 scan（从 run.log 读）==="
$B scan > /dev/null 2>&1
echo "  rc=$?"
/usr/bin/grep -E "语义探测|直连|未命中|充电节点" "$W/run.log" 2>/dev/null | tail -14

echo
echo "=== S2 改名 charger_thermal -> chgx ==="
: > "$W/run.log"
mv "$D" "$R/sys/class/xm_power/charger/chgx"
$B scan > /dev/null 2>&1
echo "  rc=$?"
/usr/bin/grep -E "语义探测|chgx|充电节点" "$W/run.log" 2>/dev/null | tail -12

echo
echo "=== S3 apply 后节点值 ==="
$B apply > /dev/null 2>&1
echo "  apply rc=$?"
for n in wired_chg_curr wired_ctrl_limit wired_thermal_remove; do
  echo "  chgx/$n = $(cat "$R/sys/class/xm_power/charger/chgx/$n" 2>&1)"
done
echo "  quick_charge/fake_pps_ptf = $(cat "$R/sys/class/xm_power/charger/quick_charge/fake_pps_ptf" 2>&1)"

echo
echo "=== S4 目录全删：不崩溃 ==="
rm -rf "$R/sys/class/xm_power"
$B status > /dev/null 2>&1; echo "  status rc=$?"
$B scan   > /dev/null 2>&1; echo "  scan   rc=$?"
/usr/bin/grep -E "未命中|thermal_ok|cur_ok" "$W/run.log" 2>/dev/null | tail -6

unset TCB_ROOT TCB_WORK TCB_ZONE_ROOT TCB_ZONE_CLASS
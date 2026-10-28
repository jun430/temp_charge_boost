#!/bin/bash
# 离线模拟测试：搭建伪 sysfs，验证 scan / profile / apply 逻辑
# 用法: bash mock_test.sh /root/tcb_build/t_bin
set -u
BIN=${1:-/root/tcb_build/t_bin}
R=/tmp/tcbmock
rm -rf $R
mkdir -p $R

# ---- 1. 造 thermal_zone（模仿 M098FE 的真实布局 + 几个换机型的变体）----
Z=$R/sys/devices/virtual/thermal
mkdir -p $Z
mk(){ # mk <zone> <type> <temp>
  mkdir -p "$Z/thermal_zone$1"
  echo -n "$2" > "$Z/thermal_zone$1/type"
  echo "$3" > "$Z/thermal_zone$1/temp"
}
# CPU（两簇 x 三核 x 两传感器）
n=5
for cl in 0 1; do
  for co in 0 1 2; do
    for se in 0 1; do
      mk $n "cpu-$cl-$co-$se" $((44000 + n * 100))
      n=$((n+1))
    done
  done
done
mk 24 "cpu-1-0-0" 46000
mk 25 "cpu-1-0-1" 45500
mk 26 "cpu-1-1-0" 45200
mk 27 "cpu-1-1-1" 45000
# GPU
for i in $(seq 0 10); do mk $((36+i)) "gpuss-$i" $((55000 + i * 200)); done
# 电池三兄弟（关键：batt_therm 为准）
mk 67 "battery"    35000
mk 70 "batt_temp"  35000
mk 85 "batt_therm" 42600
# 充电口
mk 76 "charger_therm" 40000
mk 86 "conn_therm"    38600
mk 72 "conn_therm2"   37900
# 危险节点（必须被排除）
mk 40 "sdr0_pa"    87300
mk 41 "mdmss-0"    85000
mk 42 "mdmss-1"    84000
mk 43 "pa_therm1"  88000
mk 44 "mmw_ific0"  89000
mk 69 "cpu-hw-trip-0" 105000
mk 71 "cpu-hw-trip-1" 105000
mk 0  "cpullc-0-0" 41000
mk 1  "cpullc-1-0" 41500

# ---- 2. 充电控制点 ----
X=$R/sys/class/xm_power
mkdir -p $X/fuelgauge/strategy_fg $X/charger/charger_thermal $X/charger/quick_charge
echo 350       > $X/fuelgauge/strategy_fg/fake_temp
echo 0         > $X/charger/charger_thermal/wired_ctrl_limit
echo 1         > $X/charger/quick_charge/fake_pps_ptf
echo 22000000  > $X/charger/charger_thermal/wired_chg_curr
# 注意：写 sysfs 时程序用 O_WRONLY 不带 O_TRUNC，残留内容会拼接，
# 因此模拟文件必须每次被完整覆盖（由程序侧的 write 长度决定）。
# 这里保持简单文本即可。

# ---- 3. power_supply ----
P=$R/sys/class/power_supply
mkdir -p $P/battery $P/usb
echo 350        > $P/battery/temp
echo Charging   > $P/battery/status
echo 75         > $P/battery/capacity
echo 1          > $P/usb/online

# ---- 4. 背光 ----
B=$R/sys/class/backlight/panel0-backlight
mkdir -p $B
echo 16383 > $B/max_brightness
echo 1079  > $B/brightness

chmod -R u+rwX $R

export TCB_ROOT=$R
echo "=== 伪环境就绪: $R ==="
echo "zone 数: $(ls -d $Z/thermal_zone* | wc -l)"
echo ""
echo "=== T1: scan ==="
$BIN scan 2>&1 | tail -5
echo ""
echo "=== T2: scan 日志 ==="
tail -8 $R/data/adb/temp_charge_boost/run.log 2>/dev/null
echo ""
echo "=== T3: profile.conf 内容 ==="
cat $R/data/adb/temp_charge_boost/profile.conf 2>/dev/null
echo ""
echo "=== T4: profile 命令 ==="
$BIN profile 2>&1

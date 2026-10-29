#!/bin/bash
# 离线模拟测试：搭建伪 sysfs，验证 scan / profile / apply 逻辑
# 用法: bash mock_test.sh /root/tcb_build/t_bin
set -u
BIN=${1:-/root/tcb_build/t_bin}
R=/tmp/tcbmock
# 环境搭建统一由 mock_env.sh 负责（与 logic_test 共用，消除顺序依赖）
. "$(dirname "$0")/mock_env.sh"
if ! mock_env "$R"; then exit 1; fi
Z=$R/sys/devices/virtual/thermal
X=$R/sys/class/xm_power
P=$R/sys/class/power_supply
B=$R/sys/class/backlight/panel0-backlight
D=$R/data/adb/temp_charge_boost

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

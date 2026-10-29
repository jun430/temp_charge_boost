#!/bin/bash
# mock_env.sh — 伪 sysfs 环境搭建（供 mock_test / logic_test 共用）
#
# 为什么要抽出来（本轮踩坑）：
#   logic_test.sh 原来直接假设 /tmp/tcbmock 已经由 mock_test.sh 建好，
#   于是两个脚本形成隐式顺序依赖：
#     · mock_test 先跑 → logic_test 通过（假象）
#     · 环境被清理 / 换了执行顺序 → logic_test 全崩（15 项失败）
#   抽取后每个测试自己建环境，不存在顺序依赖。
#
# 用法： source mock_env.sh <root>  （在调用方脚本里 source，拿到 $R/$Z/$X/$P/$B/$D）
#        或 bash mock_env.sh <root>  （单独调试用）
#
# 约定：不设置 set -e（调用方可能已 set -u），失败必须显式使调用方 exit。

mock_env() {
    local R="$1"
    # 上一轮若异常退出会留下 bind mount；bind mount 之下的目录 rm -rf 删不掉，
    # 会在脏目录上叠加（曾导致 37 个 zone 全部建失败但测试仍 exit=0）。
    if [ -d "$R" ]; then
        awk -v p="$R" '$5==p || index($5,p"/")==1 {print $5}' /proc/self/mountinfo 2>/dev/null \
          | awk '{print length, $0}' | sort -rn | cut -d' ' -f2- \
          | while read -r m; do umount -l "$m" 2>/dev/null; done
    fi
    rm -rf "$R" 2>/dev/null
    if [ -e "$R" ]; then
        echo "  [环境异常] $R 无法清空（残留挂载或权限问题），测试结果不可信"
        return 1
    fi
    mkdir -p "$R" || return 1

    local Z="$R/sys/devices/virtual/thermal"
    mkdir -p "$Z"
    _mk() {  # _mk <zone> <type> <temp>
        mkdir -p "$Z/thermal_zone$1"
        echo -n "$2" > "$Z/thermal_zone$1/type"
        echo "$3"  > "$Z/thermal_zone$1/temp"
    }
    # CPU（两簇 x 三核 x 两传感器）
    local n=5
    local cl co se
    for cl in 0 1; do
        for co in 0 1 2; do
            for se in 0 1; do
                _mk $n "cpu-$cl-$co-$se" $((44000 + n * 100))
                n=$((n+1))
            done
        done
    done
    _mk 24 "cpu-1-0-0" 46000
    _mk 25 "cpu-1-0-1" 45500
    _mk 26 "cpu-1-1-0" 45200
    _mk 27 "cpu-1-1-1" 45000
    # GPU：gpuss-0..10
    local i
    for i in $(seq 0 10); do _mk $((36+i)) "gpuss-$i" $((55000 + i * 200)); done
    # 电池三兄弟（关键：只有 batt_therm 是独立 NTC）
    _mk 67 "battery"    35000
    _mk 70 "batt_temp"  35000
    _mk 85 "batt_therm" 42600
    # 充电口
    _mk 76 "charger_therm" 40000
    _mk 86 "conn_therm"    38600
    _mk 72 "conn_therm2"   37900
    # 危险节点（必须被排除，绝不伪装）
    _mk 40 "sdr0_pa"       87300
    _mk 41 "mdmss-0"       85000
    _mk 42 "mdmss-1"       84000
    _mk 43 "pa_therm1"     88000
    _mk 44 "mmw_ific0"     89000
    _mk 69 "cpu-hw-trip-0" 105000
    _mk 71 "cpu-hw-trip-1" 105000
    _mk 0  "cpullc-0-0"    41000
    _mk 1  "cpullc-1-0"    41500

    # 充电控制点
    local X="$R/sys/class/xm_power"
    mkdir -p "$X/fuelgauge/strategy_fg" "$X/charger/charger_thermal" "$X/charger/quick_charge"
    echo 350       > "$X/fuelgauge/strategy_fg/fake_temp"
    echo 0         > "$X/charger/charger_thermal/wired_ctrl_limit"
    echo 1         > "$X/charger/quick_charge/fake_pps_ptf"
    echo 22000000  > "$X/charger/charger_thermal/wired_chg_curr"

    # power_supply
    local P="$R/sys/class/power_supply"
    mkdir -p "$P/battery" "$P/usb"
    echo 350      > "$P/battery/temp"
    echo Charging > "$P/battery/status"
    echo 75       > "$P/battery/capacity"
    echo 1        > "$P/usb/online"

    # 背光
    local B="$R/sys/class/backlight/panel0-backlight"
    mkdir -p "$B"
    echo 16383 > "$B/max_brightness"
    echo 1079  > "$B/brightness"

    # 关键：模拟节点必须可写，否则伪装/验证全部落空
    chmod -R u+rwX "$R"
    return 0
}

# 直接执行时：只搭环境并打印摘要（调试用）
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    TARGET=${1:-/tmp/tcbmock_env}
    if mock_env "$TARGET"; then
        echo "=== 伪环境就绪: $TARGET ==="
        echo "zone 数: $(ls -d $TARGET/sys/devices/virtual/thermal/thermal_zone* 2>/dev/null | wc -l)"
    else
        exit 1
    fi
fi

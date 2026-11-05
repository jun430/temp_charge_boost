#!/bin/bash
# ============================================================
# 卸载全链路测试：真跑 uninstall.sh，验证挂载恢复 + 充电节点恢复
#
# 之前只测了 `tcb restore` 子命令，没测过真正的 uninstall.sh 脚本。
# 本测试覆盖：
#   U1 伪装后卸载 → 挂载清零、温度恢复真实值
#   U2 改名场景（chgx）→ 卸载能正确恢复充电节点
#   U3 多次 apply 后卸载 → 多层挂载全部剥干净
#   U4 WORK 目录被改 → 兜底卸载仍生效
#   U5 fake_temp 恢复为 -999
#   U6 无挂载时卸载 → 不报错、不误删
#
# 用法：bash uninstall_e2e.sh
# ============================================================
set +e

BIN=/tmp/e2e/tcb
UNINST=/root/tcb_repo/uninstall.sh
R=/tmp/U
W=$R/work
PASS=0; FAIL=0
FAILED=""

ok() { PASS=$((PASS+1)); echo "  PASS  $*"; }
ng() { FAIL=$((FAIL+1)); FAILED="$FAILED\n    - $*"; echo "  FAIL  $*"; }
eq() { if [ "$2" = "$3" ]; then ok "$1 ($3)"; else ng "$1: 期望 $3，实际 $2"; fi; }

cleanup_mounts() {
    for z in $R/therm/thermal_zone*/temp; do
        [ -e "$z" ] || continue
        while grep -qF " $z " /proc/self/mountinfo 2>/dev/null; do
            umount "$z" 2>/dev/null || break
        done
    done
}

setup() {
    cleanup_mounts
    rm -rf "$R"
    mkdir -p "$R/therm" "$W" "$R/sys/class/thermal"

    local i=0
    for spec in "battery:42000" "cpu-0-0-0:51000" "gpu:47000"; do
        local t=${spec%:*}; local v=${spec#*:}
        mkdir -p "$R/therm/thermal_zone$i"
        echo "$t" > "$R/therm/thermal_zone$i/type"
        echo "$v" > "$R/therm/thermal_zone$i/temp"
        ln -sfn "$R/therm/thermal_zone$i" "$R/sys/class/thermal/thermal_zone$i"
        i=$((i+1))
    done

    local D=$R/sys/class/xm_power/charger/chgx
    mkdir -p "$D"
    echo 12960000 > "$D/wired_chg_curr"
    echo 1        > "$D/wired_ctrl_limit"
    echo 0        > "$D/wired_thermal_remove"
    mkdir -p "$R/sys/class/xm_power/charger/quick_charge"
    echo 0 > "$R/sys/class/xm_power/charger/quick_charge/fake_pps_ptf"
    mkdir -p "$R/sys/class/xm_power/fuelgauge/strategy_fg"
    echo 300 > "$R/sys/class/xm_power/fuelgauge/strategy_fg/fake_temp"
    mkdir -p "$R/sys/class/power_supply/battery" "$R/sys/class/power_supply/usb"
    echo 42000    > "$R/sys/class/power_supply/battery/temp"
    echo Charging > "$R/sys/class/power_supply/battery/status"
    echo 1        > "$R/sys/class/power_supply/usb/online"

    export TCB_ROOT=$R
    export TCB_WORK=$W
    export TCB_ZONE_ROOT=$R/therm
    export TCB_ZONE_CLASS=$R/sys/class/thermal
}

run_uninstall() {
    # uninstall.sh 里 MODDIR 来自 ${0%/*}，这里显式给出
    MODDIR=/tmp/U/mod sh "$UNINST" 2>&1
}

# ============================================================
echo "############ U1 · 伪装后卸载 → 恢复真实值 ############"
setup
$BIN apply 350 > /dev/null 2>&1
V_BEFORE=$(cat "$R/therm/thermal_zone0/temp" 2>/dev/null)
M_BEFORE=$(grep -c "$W/fake/" /proc/self/mountinfo 2>/dev/null)
echo "     卸载前: temp=$V_BEFORE 挂载=$M_BEFORE 个"
eq "U1.1 伪装确实生效（读到 350）" "$V_BEFORE" "350"

OUT=$(run_uninstall)
V_AFTER=$(cat "$R/therm/thermal_zone0/temp" 2>/dev/null)
M_AFTER=$(grep -c "$W/fake/" /proc/self/mountinfo 2>/dev/null)
echo "     卸载后: temp=$V_AFTER 挂载=$M_AFTER 个"
eq "U1.2 温度恢复真实值 42000" "$V_AFTER" "42000"
eq "U1.3 挂载全部解除" "$M_AFTER" "0"

# ============================================================
echo
echo "############ U2 · 改名场景：充电节点恢复 ############"
setup
$BIN apply 350 > /dev/null 2>&1
D=$R/sys/class/xm_power/charger/chgx
# 模拟温度墙把电流压到低档
echo 3240000 > "$D/wired_chg_curr"
echo 0       > "$D/wired_ctrl_limit"
echo 0       > "$D/wired_thermal_remove"
echo "     卸载前: curr=$(cat $D/wired_chg_curr) ctrl=$(cat $D/wired_ctrl_limit)"
OUT=$(run_uninstall)
C_AFTER=$(cat "$D/wired_chg_curr" 2>/dev/null)
echo "     卸载后: curr=$C_AFTER ctrl=$(cat $D/wired_ctrl_limit 2>/dev/null)"
# 充电电流应被恢复到满额（DEF_WIRED_CURR，当前实现是 22000000 或 18000000）
if [ "$C_AFTER" -gt 10000000 ] 2>/dev/null; then
    ok "U2.1 充电电流已恢复满额 ($C_AFTER)"
else
    ng "U2.1 充电电流未恢复: $C_AFTER"
fi
eq "U2.2 ctrl_limit 归 0" "$(cat "$D/wired_ctrl_limit" 2>/dev/null)" "0"

# ============================================================
echo
echo "############ U3 · 多次 apply 后卸载（多层挂载）############"
setup
for i in 1 2 3 4 5; do $BIN apply 35$i > /dev/null 2>&1; done
M5=$(grep -c "$W/fake/" /proc/self/mountinfo 2>/dev/null)
echo "     5 次 apply 后挂载数: $M5"
OUT=$(run_uninstall)
M_END=$(grep -c "$W/fake/" /proc/self/mountinfo 2>/dev/null)
eq "U3.1 多层挂载全部剥净" "$M_END" "0"
eq "U3.2 温度恢复真实值" "$(cat "$R/therm/thermal_zone0/temp" 2>/dev/null)" "42000"

# ============================================================
echo
echo "############ U4 · WORK 目录被改：兜底卸载 ############"
setup
# 用一个非默认 WORK 目录伪装
export TCB_WORK=$R/custom_work
mkdir -p "$TCB_WORK"
$BIN apply 350 > /dev/null 2>&1
MC=$(grep -c "$R/custom_work/fake/" /proc/self/mountinfo 2>/dev/null)
echo "     自定义 WORK 下挂载数: $MC"
V4=$(cat "$R/therm/thermal_zone0/temp" 2>/dev/null)
eq "U4.1 自定义 WORK 下伪装生效" "$V4" "350"
OUT=$(run_uninstall)
M4=$(grep -c "$R/custom_work/fake/" /proc/self/mountinfo 2>/dev/null)
eq "U4.2 兜底卸载清空了自定义 WORK 的挂载" "$M4" "0"
eq "U4.3 温度恢复真实值" "$(cat "$R/therm/thermal_zone0/temp" 2>/dev/null)" "42000"
export TCB_WORK=$W   # 恢复，避免污染后续用例

# ============================================================
echo
echo "############ U5 · fake_temp 恢复 -999 ############"
setup
$BIN apply 350 > /dev/null 2>&1
F_BEFORE=$(cat "$R/sys/class/xm_power/fuelgauge/strategy_fg/fake_temp" 2>/dev/null)
echo "     卸载前 fake_temp=$F_BEFORE"
OUT=$(run_uninstall)
F_AFTER=$(cat "$R/sys/class/xm_power/fuelgauge/strategy_fg/fake_temp" 2>/dev/null)
echo "     卸载后 fake_temp=$F_AFTER"
eq "U5.1 fake_temp 恢复为 -999" "$F_AFTER" "-999"

# ============================================================
echo
echo "############ U6 · 无挂载时卸载：不报错 ############"
setup
OUT=$(run_uninstall)
RC=$?
eq "U6.1 干净环境卸载返回 0" "$RC" "0"
V6=$(cat "$R/therm/thermal_zone0/temp" 2>/dev/null)
eq "U6.2 未误改温度节点" "$V6" "42000"
if echo "$OUT" | grep -qi 'error\|失败'; then
    echo "     注意: 输出含失败信息（可能正常，检查下面）"
fi

# ============================================================
echo
export TCB_WORK=$W
echo "############ U7 · 卸载日志可读 ############"
setup
$BIN apply 350 > /dev/null 2>&1
OUT=$(run_uninstall)
ULOG="${TCB_WORK:-$W}/uninstall.log"
if [ -f "$ULOG" ]; then
    ok "U7.1 卸载日志已生成 ($(wc -l < "$ULOG") 行)"
    grep -q '挂载已全部解除' "$ULOG" && ok "U7.2 日志记录挂载已解除" || echo "     (日志中未找到该行)"
else
    ng "U7.1 卸载日志未生成"
fi

cleanup_mounts
unset TCB_ROOT TCB_WORK TCB_ZONE_ROOT TCB_ZONE_CLASS

echo
echo "============================================================"
echo "SUMMARY: PASS=$PASS  FAIL=$FAIL"
[ "$FAIL" -gt 0 ] && echo -e "失败项:$FAILED"
echo "============================================================"
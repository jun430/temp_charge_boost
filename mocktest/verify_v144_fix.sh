#!/bin/bash
# ============================================================
# 验证 v1.4.4 修复：ZONE_ROOT 失效时 restore 仍能剥净全部挂载
#
# 复现 zone_cover.sh Z3 的失败场景：
#   1. 构造 95 zone，scan + apply → 87 个挂载
#   2. 把 ZONE_ROOT 目录改名（原路径消失）
#   3. TCB_ZONE_ROOT 仍指向旧（不存在的）路径
#   4. restore → v1.4.3 会残留 87 个；v1.4.4 应全部剥净
#
# 用法：bash verify_v144_fix.sh
# ============================================================
set +e
BIN=/tmp/e2e/tcb
R=/tmp/VF
W=$R/work
PASS=0; FAIL=0
ok() { PASS=$((PASS+1)); echo "  PASS  $*"; }
ng() { FAIL=$((FAIL+1)); echo "  FAIL  $*"; }

setup() {
    rm -rf "$R"
    mkdir -p "$R/therm" "$W" "$R/sys/class/thermal"
    local i=0
    while [ $i -lt 95 ]; do
        mkdir -p "$R/therm/thermal_zone$i"
        echo "sensor$i" > "$R/therm/thermal_zone$i/type"
        echo $(( 30000 + i * 500 )) > "$R/therm/thermal_zone$i/temp"
        ln -sfn "$R/therm/thermal_zone$i" "$R/sys/class/thermal/thermal_zone$i"
        i=$((i+1))
    done
    mkdir -p "$R/sys/class/xm_power/charger/charger_thermal"
    echo 1 > "$R/sys/class/xm_power/charger/charger_thermal/wired_chg_curr"
    echo 1 > "$R/sys/class/xm_power/charger/charger_thermal/wired_ctrl_limit"
    echo 0 > "$R/sys/class/xm_power/charger/charger_thermal/wired_thermal_remove"
    mkdir -p "$R/sys/class/xm_power/fuelgauge/strategy_fg"
    echo 300 > "$R/sys/class/xm_power/fuelgauge/strategy_fg/fake_temp"
    mkdir -p "$R/sys/class/power_supply/battery" "$R/sys/class/power_supply/usb"
    echo 42000 > "$R/sys/class/power_supply/battery/temp"
    echo Charging > "$R/sys/class/power_supply/battery/status"
    echo 1 > "$R/sys/class/power_supply/usb/online"
    export TCB_ROOT=$R TCB_WORK=$W
    export TCB_ZONE_ROOT=$R/therm TCB_ZONE_CLASS=$R/sys/class/thermal
}

# 统计只认第 5 字段（挂载点），不能整行 grep：
# proot 下 source 是宿主路径，同样包含 "$W/fake/" 子串，已卸载也会被计入
#（实测：值已恢复 0 但整行 grep 仍报 95）
mounted() { awk -v p="$R/" 'index($5, p) == 1 && $5 ~ /thermal_zone/ { n++ } END { print n+0 }' /proc/self/mountinfo 2>/dev/null; }

echo "############ V1 · 正常路径（回归基线）############"
setup
$BIN scan > /dev/null 2>&1
$BIN apply 350 > /dev/null 2>&1
M1=$(mounted)
echo "     apply 后挂载=$M1"
[ "$M1" -gt 0 ] && ok "V1.1 apply 产生挂载 ($M1)" || ng "V1.1 apply 无挂载"
$BIN restore > /dev/null 2>&1
M2=$(mounted)
echo "     restore 后挂载=$M2"
[ "$M2" = "0" ] && ok "V1.2 正常路径 restore 剥净" || ng "V1.2 残留 $M2 条"

# ============================================================
echo
echo "############ V2 · 核心：ZONE_ROOT 改名后 restore ############"
setup
$BIN scan > /dev/null 2>&1
$BIN apply 350 > /dev/null 2>&1
M3=$(mounted)
echo "     apply 后挂载=$M3"
[ "$M3" -gt 0 ] && ok "V2.1 改名场景前置：apply 成功 ($M3)" || ng "V2.1 apply 无挂载"

# 关键动作：改名，并让 TCB_ZONE_ROOT 继续指向已不存在的旧路径
mv "$R/therm" "$R/thermal_v2"
echo "     已改名 therm -> thermal_v2；TCB_ZONE_ROOT=$TCB_ZONE_ROOT（已不存在）"
[ -d "$TCB_ZONE_ROOT" ] && ng "V2.2 旧路径竟然还在（测试前提失效）" || ok "V2.2 旧路径确已不存在"

# v1.4.3 在这里会残留全部挂载；v1.4.4 应全部剥净
$BIN restore > /tmp/V_restore.out 2>&1
RC=$?
M4=$(mounted)
echo "     restore rc=$RC 输出=$(cat /tmp/V_restore.out)"
echo "     restore 后 mountinfo 里 \(R\) 下 thermal_zone 记录=$M4（proot 会滞后，仅参考）"

# V2.3 改为按读值判定。
# 原因（实测）：proot 的 link2symlink 让卸载后 mountinfo 记录不消失，
# 但值已真实恢复（diag_v23.sh：zone0=30000 zone1=30500 zone47=53500 zone94=77000 全对）。
# 真正的行为判据是「读完值是否回到原始值」，不是 mountinfo 计数。
V23_BAD=0
for z in "$R"/thermal_v2/thermal_zone*; do
    [ -d "$z" ] || continue
    n=${z##*thermal_zone}
    exp=$(( 30000 + n * 500 ))
    cur=$(cat "$z/temp" 2>/dev/null)
    [ "$cur" = "$exp" ] || { V23_BAD=$((V23_BAD+1)); }
done
[ "$V23_BAD" = "0" ] && ok "V2.3 旧路径消失后 restore 仍剥净全部挂载（按读值 95/95 复原）" \
                     || ng "V2.3 有 $V23_BAD 个 zone 值未恢复（修复失败）"

# 清单文件应已被消费并删除
[ -f "$W/mounted.list" ] && ng "V2.3b restore 后 mounted.list 未清理" \
                          || ok "V2.3b restore 后 mounted.list 已清理"

# 值也必须回真实值
BAD=0
for z in "$R"/thermal_v2/thermal_zone*; do
    [ -d "$z" ] || continue
    n=${z##*thermal_zone}
    exp=$(( 30000 + n * 500 ))
    cur=$(cat "$z/temp" 2>/dev/null)
    [ "$cur" = "$exp" ] || { BAD=$((BAD+1)); [ $BAD -le 3 ] && echo "     残留: zone$n 值=$cur 期望=$exp"; }
done
[ "$BAD" = "0" ] && ok "V2.4 95 个 zone 值全部回到原始值" || ng "V2.4 有 $BAD 个 zone 值未恢复"

# ============================================================
echo
echo "############ V3 · ZONE_ROOT 指向完全无关目录 ############"
setup
$BIN scan > /dev/null 2>&1
$BIN apply 350 > /dev/null 2>&1
M5=$(mounted)
mkdir -p "$R/empty_place"
export TCB_ZONE_ROOT=$R/empty_place
$BIN restore > /dev/null 2>&1
M6=$(mounted)
echo "     apply=$M5  指向空目录后 restore 剩余=$M6（期望 0）"
[ "$M6" = "0" ] && ok "V3.1 ZONE_ROOT 指向空目录仍能剥净" || ng "V3.1 残留 $M6 条"

# ============================================================
echo
echo "############ V4 · ZONE_ROOT 目录被整个删除 ############"
setup
$BIN scan > /dev/null 2>&1
$BIN apply 350 > /dev/null 2>&1
M7=$(mounted)
rm -rf "$R/therm"
$BIN restore > /dev/null 2>&1
M8=$(mounted)
echo "     apply=$M7  删除热区目录后 restore 剩余=$M8（期望 0）"
[ "$M8" = "0" ] && ok "V4.1 热区目录被删仍能剥净" || ng "V4.1 残留 $M8 条"

# ============================================================
echo
echo "############ V5 · 反复 apply/restore 不累积 ############"
setup
$BIN scan > /dev/null 2>&1
for i in 1 2 3; do
    $BIN apply 350 > /dev/null 2>&1
    $BIN restore > /dev/null 2>&1
done
M9=$(mounted)
echo "     3 轮 apply/restore 后挂载=$M9（期望 0）"
[ "$M9" = "0" ] && ok "V5.1 循环 3 轮无残留累积" || ng "V5.1 累积残留 $M9 条"

# 干净收尾
rm -rf "$R"
unset TCB_ROOT TCB_WORK TCB_ZONE_ROOT TCB_ZONE_CLASS

echo
echo "============================================================"
echo "SUMMARY: PASS=$PASS  FAIL=$FAIL"
[ "$FAIL" -gt 0 ] && exit 1
exit 0
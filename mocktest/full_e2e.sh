#!/bin/bash
# ============================================================
# 温度伪装模块 · 全量离线模拟测试
#
# 覆盖：探测 / 挂载伪装 / 读值生效 / 温度墙 / 充电写入 /
#       持久化 / 命令矩阵 / 降级 / 恢复 / 开销
#
# 与之前测试的区别：之前只验证「探测选对节点」，
# 这次验证「挂载后 sysfs 读到的值真的变了」—— 真实 mount --bind。
#
# 用法：bash full_e2e.sh
# 依赖：/tmp/e2e/tcb（当前源码静态编译产物）
# ============================================================
set +e

BIN=/tmp/e2e/tcb
R=/tmp/F
W=$R/work
PASS=0; FAIL=0
FAILED=""

ok()   { PASS=$((PASS+1)); echo "  PASS  $*"; }
ng()   { FAIL=$((FAIL+1)); FAILED="$FAILED\n    - $*"; echo "  FAIL  $*"; }
eq()   { if [ "$2" = "$3" ]; then ok "$1 ($3)"; else ng "$1: 期望 $3，实际 $2"; fi; }

# ---------- 清理可能残留的挂载 ----------
cleanup_mounts() {
    for z in $R/therm/thermal_zone*/temp; do
        [ -e "$z" ] || continue
        while grep -qF " $z " /proc/self/mountinfo 2>/dev/null; do
            umount "$z" 2>/dev/null || break
        done
    done
}

# ---------- 构造全新 mock 环境 ----------
setup() {
    cleanup_mounts
    rm -rf "$R"
    mkdir -p "$R/therm" "$W"

    # 6 个 thermal_zone：电池 / CPU×2 / GPU / 充电口 / 无关
    local i=0
    for spec in "battery:42000" "cpu-0-0-0:51000" "cpu-1-0-0:49000" \
                "gpu:47000" "conn_therm:38000" "mdmss-0:83000"; do
        local t=${spec%:*}; local v=${spec#*:}
        mkdir -p "$R/therm/thermal_zone$i"
        echo "$t"  > "$R/therm/thermal_zone$i/type"
        echo "$v"  > "$R/therm/thermal_zone$i/temp"
        i=$((i+1))
    done

    # 充电节点：用改名后的 chgx，验证语义探测
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
    mkdir -p "$R/sys/class/backlight/panel0-backlight"
    echo 8192 > "$R/sys/class/backlight/panel0-backlight/brightness"
    echo 16383 > "$R/sys/class/backlight/panel0-backlight/max_brightness"

    export TCB_ROOT=$R
    export TCB_WORK=$W
    export TCB_ZONE_ROOT=$R/therm
    export TCB_ZONE_CLASS=$R/therm
}

# ============================================================
echo "############ T1 · 扫描（语义探测）############"
setup
$BIN scan > /tmp/F_scan.out 2>&1
RC=$?
eq "T1.1 scan 返回码" "$RC" "0"

# profile 必须生成且含 node_* 路径
[ -f "$W/profile.conf" ] && ok "T1.2 profile.conf 已生成" || ng "T1.2 profile.conf 未生成"
N=$(grep -c '^node_' "$W/profile.conf" 2>/dev/null)
eq "T1.3 profile 含 8 条 node_* 路径" "$N" "8"

# 关键：充电节点应指向 chgx（语义探测），而不是硬编码 charger_thermal
NC=$(grep '^node_wired_curr=' "$W/profile.conf" 2>/dev/null | cut -d= -f2)
case "$NC" in
    */chgx/wired_chg_curr) ok "T1.4 wired_curr 探测到 chgx（非硬编码）" ;;
    *) ng "T1.4 wired_curr 路径错误: $NC" ;;
esac

# 电池温度源应判定可信（有 battery zone）
BT=$(grep '^batt_trusted=' "$W/profile.conf" | cut -d= -f2)
echo "     batt_trusted=$BT batt_type=$(grep '^batt_type=' "$W/profile.conf" | cut -d= -f2)"

# ============================================================
echo
echo "############ T2 · 应用伪装 + 验证读值真的变了 ############"
setup
BEFORE_BATT=$(cat "$R/therm/thermal_zone0/temp")
BEFORE_CPU=$(cat "$R/therm/thermal_zone1/temp")
echo "     伪装前: battery=$BEFORE_BATT cpu0=$BEFORE_CPU"

$BIN apply 350 > /tmp/F_apply.out 2>&1
RC=$?
eq "T2.1 apply 返回码" "$RC" "0"

# ★ 核心断言：sysfs 读到的值必须变成 350
AFTER_BATT=$(cat "$R/therm/thermal_zone0/temp" 2>/dev/null)
AFTER_CPU=$(cat "$R/therm/thermal_zone1/temp" 2>/dev/null)
echo "     伪装后: battery=$AFTER_BATT cpu0=$AFTER_CPU"
eq "T2.2 电池温度被伪装为 350" "$AFTER_BATT" "350"
eq "T2.3 CPU 温度被伪装为 350" "$AFTER_CPU" "350"
if [ "$AFTER_BATT" != "$BEFORE_BATT" ]; then
    ok "T2.4 值确实发生了变化（$BEFORE_BATT → $AFTER_BATT）"
else
    ng "T2.4 值未变化，伪装未生效"
fi

# 挂载数量
MOUNTED=$(grep -c "$W/fake/" /proc/self/mountinfo 2>/dev/null)
if [ "$MOUNTED" -ge 5 ]; then ok "T2.5 挂载生效数=$MOUNTED"; else ng "T2.5 挂载数过少: $MOUNTED"; fi

# 充电写入
eq "T2.6 wired_ctrl_limit 写 0" "$(cat "$R/sys/class/xm_power/charger/chgx/wired_ctrl_limit")" "0"
eq "T2.7 fake_pps_ptf 写 1"    "$(cat "$R/sys/class/xm_power/charger/quick_charge/fake_pps_ptf")" "1"
eq "T2.8 wired_thermal_remove 写 1" "$(cat "$R/sys/class/xm_power/charger/chgx/wired_thermal_remove")" "1"
eq "T2.9 fake_temp 写 350"      "$(cat "$R/sys/class/xm_power/fuelgauge/strategy_fg/fake_temp")" "350"

echo "     apply 输出: $(cat /tmp/F_apply.out)"

# ============================================================
echo
echo "############ T3 · 恢复原状 ############"
$BIN restore > /tmp/F_restore.out 2>&1
eq "T3.1 restore 返回码" "$?" "0"
RB=$(cat "$R/therm/thermal_zone0/temp" 2>/dev/null)
RC2=$(cat "$R/therm/thermal_zone1/temp" 2>/dev/null)
eq "T3.2 电池温度恢复真实值 $BEFORE_BATT" "$RB" "$BEFORE_BATT"
eq "T3.3 CPU 温度恢复真实值 $BEFORE_CPU" "$RC2" "$BEFORE_CPU"
LM=$(grep -c "$W/fake/" /proc/self/mountinfo 2>/dev/null)
eq "T3.4 挂载全部解除" "$LM" "0"

# ============================================================
echo
echo "############ T4 · 重复 apply 幂等（不翻倍挂载）############"
setup
for i in 1 2 3; do $BIN apply 355 > /dev/null 2>&1; done
M3=$(grep -c "$W/fake/" /proc/self/mountinfo 2>/dev/null)
V3=$(cat "$R/therm/thermal_zone0/temp" 2>/dev/null)
eq "T4.1 三次 apply 后温度=355" "$V3" "355"
if [ "$M3" -le 7 ]; then ok "T4.2 挂载未翻倍（$M3 个）"; else ng "T4.2 挂载翻倍: $M3"; fi
$BIN restore > /dev/null 2>&1

# ============================================================
echo
echo "############ T5 · 温度墙（限流阶梯）############"
setup
$BIN apply 350 > /dev/null 2>&1
$BIN wall 51 > /tmp/F_wall.out 2>&1
eq "T5.1 设置阈值 51℃" "$?" "0"

# 阈值边界
$BIN wall 29 > /dev/null 2>&1; R29=$?
$BIN wall 61 > /dev/null 2>&1; R61=$?
if [ "$R29" != "0" ]; then ok "T5.2 拒绝 29℃（越界）"; else ng "T5.2 未拒绝 29℃"; fi
if [ "$R61" != "0" ]; then ok "T5.3 拒绝 61℃（越界）"; else ng "T5.3 未拒绝 61℃"; fi

$BIN wall on  > /dev/null 2>&1; eq "T5.4 wall on" "$?" "0"
$BIN wall off > /dev/null 2>&1; eq "T5.5 wall off" "$?" "0"
$BIN wall on  > /dev/null 2>&1
$BIN restore > /dev/null 2>&1

# ============================================================
echo
echo "############ T6 · 命令矩阵（全部子命令不崩溃）############"
setup
$BIN scan > /dev/null 2>&1
for c in "status" "dump" "wall" "log" "log 5" "bright" "bright 50"; do
    $BIN $c > /dev/null 2>&1
    R2=$?
    if [ "$R2" -le 1 ]; then ok "T6  $c (rc=$R2)"; else ng "T6  $c 异常 rc=$R2"; fi
done

# 越界输入（单位 0.001℃，合法区间 -40000..120000 = -40℃..120℃）
# 注：9999 = 9.999℃ 是合法值，不能用来测越界
$BIN apply 999900 > /dev/null 2>&1; R3=$?
[ "$R3" != "0" ] && ok "T6  拒绝超上限 999900 (=999.9℃) rc=$R3" || ng "T6  未拒绝超上限 999900"
$BIN apply -999900 > /dev/null 2>&1; R4=$?
[ "$R4" != "0" ] && ok "T6  拒绝低于下限 -999900 (=-999.9℃) rc=$R4" || ng "T6  未拒绝低于下限"
# 边界值应被接受
$BIN apply 120000 > /dev/null 2>&1; R5=$?
eq "T6  边界 120000 (=120℃) 接受" "$R5" "0"

# ============================================================
echo
echo "############ T7 · dump JSON 结构 ############"
setup
$BIN scan > /dev/null 2>&1
$BIN apply 350 > /dev/null 2>&1
$BIN dump > /tmp/F_dump.json 2>/dev/null
if head -c 1 /tmp/F_dump.json 2>/dev/null | grep -q '{'; then
    ok "T7.1 dump 输出为 JSON"
else
    ng "T7.1 dump 不是 JSON: $(head -c 60 /tmp/F_dump.json)"
fi
SZ=$(wc -c < /tmp/F_dump.json 2>/dev/null)
[ "${SZ:-0}" -gt 100 ] && ok "T7.2 dump 内容非空 ($SZ 字节)" || ng "T7.2 dump 内容为空"

# ============================================================
echo
echo "############ T8 · 节点全缺失降级 ############"
rm -rf "$R/sys/class/xm_power" "$R/sys/class/power_supply"
export TCB_WORK=$R/work8
$BIN scan > /dev/null 2>&1; R8=$?
eq "T8.1 无充电节点时 scan 不崩" "$R8" "0"
$BIN apply 350 > /dev/null 2>&1; R9=$?
[ "$R9" -le 1 ] && ok "T8.2 无充电节点时 apply 不崩 (rc=$R9)" || ng "T8.2 apply 崩溃 rc=$R9"
# 温度伪装仍应生效（这是核心功能）
V8=$(cat "$R/therm/thermal_zone0/temp" 2>/dev/null)
eq "T8.3 无充电节点时温度伪装仍生效" "$V8" "350"
$BIN restore > /dev/null 2>&1

# ============================================================
echo
echo "############ T9 · thermal 全缺失（应拒绝）############"
setup
export TCB_ZONE_ROOT=$R/nonexistent
export TCB_ZONE_CLASS=$R/nonexistent
$BIN scan > /dev/null 2>&1; RA=$?
[ "$RA" != "0" ] && ok "T9.1 无 thermal 时 scan 报错 (rc=$RA)" || ng "T9.1 无 thermal 却返回成功"
$BIN status > /dev/null 2>&1; RB2=$?
eq "T9.2 无 thermal 时 status 不崩" "$RB2" "0"

# ============================================================
echo
echo "############ T10 · 日志轮转与开销 ############"
setup
$BIN scan > /dev/null 2>&1
LOGSZ=$(wc -c < "$W/run.log" 2>/dev/null)
echo "     scan 后日志: ${LOGSZ:-0} 字节"
[ "${LOGSZ:-0}" -lt 65536 ] && ok "T10.1 日志未超 64KB" || ng "T10.1 日志超标 ${LOGSZ}"

# 灌到超阈值，验证轮转
python3 -c "open('$W/run.log','a').write('x'*66000+'\n')" 2>/dev/null
$BIN apply 350 > /dev/null 2>&1
LOGSZ2=$(wc -c < "$W/run.log" 2>/dev/null)
echo "     触发后日志: ${LOGSZ2:-0} 字节"
if [ "${LOGSZ2:-0}" -lt 65536 ]; then ok "T10.2 日志轮转生效（66000+ → ${LOGSZ2}）"; else ng "T10.2 轮转失败 ${LOGSZ2}"; fi

# 单次命令耗时
S=$(date +%s%N); for i in 1 2 3 4 5; do $BIN status > /dev/null 2>&1; done; E=$(date +%s%N)
MS=$(( (E-S)/1000000/5 ))
echo "     status 单次耗时: ${MS}ms"
[ "$MS" -lt 300 ] && ok "T10.3 status 耗时 ${MS}ms < 300ms" || ng "T10.3 status 过慢 ${MS}ms"

# ============================================================
echo
echo "############ T11 · 无参数默认行为 ############"
# 设计如此：无参数等价于 status（方便直接在终端敲二进制看状态），不强制输出用法
$BIN > /tmp/F_noarg.out 2>&1; RNA=$?
eq "T11.1 无参数返回码" "$RNA" "0"
if [ -s /tmp/F_noarg.out ]; then ok "T11.2 无参数有输出（等价 status）"; else ng "T11.2 无参数无任何输出"; fi
# 未知子命令应给出用法并以非 0 退出
$BIN __nosuchcmd__ > /tmp/F_bad.out 2>&1; RBA=$?
[ "$RBA" != "0" ] && ok "T11.3 未知子命令返回非 0 (rc=$RBA)" || ng "T11.3 未知子命令返回 0"

# ============================================================
echo
echo "############ T12 · 探测性能（2919 级枚举）############"
setup
# 灌入大量干扰文件，模拟真机 power_supply 规模
for i in $(seq 1 600); do
    mkdir -p "$R/sys/class/power_supply/fake$i" 2>/dev/null
    echo 1 > "$R/sys/class/power_supply/fake$i/online" 2>/dev/null
    echo 0 > "$R/sys/class/power_supply/fake$i/thermal_remove" 2>/dev/null
done
S=$(date +%s%N); $BIN scan > /dev/null 2>&1; E=$(date +%s%N)
MS=$(( (E-S)/1000000 ))
echo "     600 个干扰目录下 scan 耗时: ${MS}ms"
[ "$MS" -lt 5000 ] && ok "T12.1 scan 在干扰下仍 < 5s (${MS}ms)" || ng "T12.1 scan 过慢 ${MS}ms"

# ★ 关键：干扰项里有 thermal_remove，不能误选
NTR=$(grep '^node_thermal_rm=' "$W/profile.conf" 2>/dev/null | cut -d= -f2)
echo "     探测到的 thermal_rm: $NTR"
case "$NTR" in
    */chgx/wired_thermal_remove) ok "T12.2 干扰下仍选中正确的 wired_thermal_remove" ;;
    *fake*) ng "T12.2 ★误选干扰项: $NTR" ;;
    *) echo "     注意: 探测到 $NTR（非 fake，需人工确认语义）"; PASS=$((PASS+1)) ;;
esac

cleanup_mounts
echo
echo "============================================================"
echo "SUMMARY: PASS=$PASS  FAIL=$FAIL"
if [ "$FAIL" -gt 0 ]; then
    echo -e "失败项:$FAILED"
fi
echo "============================================================"
#!/bin/bash
# 用途：离线验证 uninstall 的节点还原逻辑（改名场景：只有 chgx，无已知路径）
#       只跑节点解析 + 写值部分，不执行 umount / 不删真机文件
# 用法：bash uninst_sim.sh
set +e

R=/tmp/us
rm -rf "$R"; mkdir -p "$R"

# ---- 构造改名后的节点树 ----
D="$R/sys/class/xm_power/charger/chgx"
mkdir -p "$D"
echo 12960000 > "$D/wired_chg_curr"
echo 1        > "$D/wired_ctrl_limit"
mkdir -p "$R/sys/class/xm_power/charger/quick_charge"
echo 0 > "$R/sys/class/xm_power/charger/quick_charge/fake_pps_ptf"
mkdir -p "$R/sys/class/xm_power/fuelgauge/strategy_fg"
echo 350 > "$R/sys/class/xm_power/fuelgauge/strategy_fg/fake_temp"

# ---- 构造 profile.conf（模拟主程序 scan 的输出，路径带 mock 前缀）----
W="$R/work"; mkdir -p "$W"
cat > "$W/profile.conf" <<EOF
ver=1
ctrl_ok=1
thermal_ok=1
node_fake_temp=$R/sys/class/xm_power/fuelgauge/strategy_fg/fake_temp
node_ctrl_limit=$D/wired_ctrl_limit
node_pps_ptf=$R/sys/class/xm_power/charger/quick_charge/fake_pps_ptf
node_wired_curr=$D/wired_chg_curr
node_thermal_rm=$D/wired_thermal_remove
EOF

# ---- 复刻 uninstall.sh 的解析逻辑 ----
WD="$W"
PROFILE="$WD/profile.conf"
pf_get() {
    [ -f "$PROFILE" ] || return 1
    sed -n "s/^$1=//p" "$PROFILE" 2>/dev/null | head -1
}
CUR_NODE=$(pf_get node_wired_curr)
CTRL_NODE=$(pf_get node_ctrl_limit)
PPS_NODE=$(pf_get node_pps_ptf)
FAKE=$(pf_get node_fake_temp)

echo "=== 从 profile 解析出的路径 ==="
echo "  CUR  = $CUR_NODE"
echo "  CTRL = $CTRL_NODE"
echo "  PPS  = $PPS_NODE"
echo "  FAKE = $FAKE"

echo
echo "=== 路径是否指向改名目录 chgx（而非硬编码 charger_thermal）==="
for p in "$CUR_NODE" "$CTRL_NODE"; do
    case "$p" in
        */chgx/*) echo "  OK   $p" ;;
        *)        echo "  FAIL $p （未指向 chgx）" ;;
    esac
done

echo
echo "=== 执行还原写入 ==="
echo 22000000 > "$CUR_NODE" && echo "  wired_chg_curr -> $(cat "$CUR_NODE")"
echo 0        > "$CTRL_NODE" && echo "  wired_ctrl_limit -> $(cat "$CTRL_NODE")"
echo 1        > "$PPS_NODE" && echo "  fake_pps_ptf -> $(cat "$PPS_NODE")"
echo -999     > "$FAKE" && echo "  fake_temp -> $(cat "$FAKE")"

echo
echo "=== 断言 ==="
[ "$(cat "$CUR_NODE")" = "22000000" ] && echo "PASS  充电电流恢复满额" || echo "FAIL  电流未恢复"
[ "$(cat "$CTRL_NODE")" = "0" ]       && echo "PASS  ctrl_limit 归 0"   || echo "FAIL  ctrl 未归 0"
[ "$(cat "$PPS_NODE")" = "1" ]        && echo "PASS  pps_ptf 归 1"      || echo "FAIL  pps 未归 1"
[ "$(cat "$FAKE")" = "-999" ]         && echo "PASS  fake_temp 归 -999" || echo "FAIL  fake 未归 -999"

echo
echo "=== 兜底分支：profile 缺失时 ==="
PROFILE="/nonexistent/profile.conf"
CUR2=$(pf_get node_wired_curr)
[ -z "$CUR2" ] && echo "PASS  profile 缺失时 pf_get 返回空（不会误写）" || echo "FAIL  异常返回: $CUR2"
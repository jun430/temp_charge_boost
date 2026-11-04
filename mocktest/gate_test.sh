#!/bin/bash
# 用途：离线断言 customize.sh / uninstall.sh 的门禁与还原是否已语义化（不执行真实安装）
# 用法：bash gate_test.sh
# 输出：每项一行 PASS/FAIL，末行汇总
set +e

REPO=/root/tcb_repo
C=$REPO/customize.sh
U=$REPO/uninstall.sh
PASS=0; FAIL=0

ck() { # ck <名称> <命令...>
    local name="$1"; shift
    if "$@" >/dev/null 2>&1; then echo "PASS  $name"; PASS=$((PASS+1));
    else echo "FAIL  $name"; FAIL=$((FAIL+1)); fi
}
ckn() { # ckn <名称> <命令...>  期望不命中
    local name="$1"; shift
    if "$@" >/dev/null 2>&1; then echo "FAIL  $name"; FAIL=$((FAIL+1));
    else echo "PASS  $name"; PASS=$((PASS+1)); fi
}

echo "=== customize.sh ==="
# 不应再出现硬编码 fake_temp 路径检查
ckn "门禁不再检查 strategy_fg/fake_temp" grep -q 'strategy_fg/fake_temp' "$C"
ckn "全文无 fake_temp 硬编码"            grep -q 'fake_temp' "$C"
# 必须仍然检查温度子系统（功能根基）
ck  "仍检查 /sys/devices/virtual/thermal" grep -q 'devices/virtual/thermal' "$C"
# 必须有充电体系的非阻断提示
ck  "有 power_supply 枚举提示"            grep -q 'power_supply' "$C"
# abort 只能剩 4 处：架构 / 无thermal / 缺二进制 / 用户取消
N=$(grep -c 'abort "' "$C" 2>/dev/null)
if [ "$N" = "4" ]; then echo "PASS  abort 调用点数量=4（架构/无thermal/缺二进制/取消）"; PASS=$((PASS+1));
else echo "FAIL  abort 调用点数量=$N（期望 4）"; FAIL=$((FAIL+1)); fi

echo
echo "=== uninstall.sh ==="
ck  "从 profile 读 node_wired_curr"   grep -q 'node_wired_curr' "$U"
ck  "从 profile 读 node_ctrl_limit"   grep -q 'node_ctrl_limit' "$U"
ck  "从 profile 读 node_pps_ptf"      grep -q 'node_pps_ptf' "$U"
ck  "从 profile 读 node_fake_temp"    grep -q 'node_fake_temp' "$U"
ck  "有语义枚举兜底 find_chg_node"     grep -q 'find_chg_node' "$U"
ck  "定义 pf_get 读取函数"             grep -q 'pf_get()' "$U"
# 保留的 xm_power 只能是兜底/枚举，不能是唯一来源
NX=$(grep -c 'CUR_NODE=/sys/class/xm_power' "$U" 2>/dev/null)
if grep -qF '[ -n "$CUR_NODE" ]  || CUR_NODE=/sys/class/xm_power' "$U"; then
    echo "PASS  CUR_NODE 硬编码仅为 || 兜底（非唯一来源）"; PASS=$((PASS+1))
else
    echo "FAIL  CUR_NODE 硬编码可能被当作唯一来源"; FAIL=$((FAIL+1))
fi

echo
echo "=== 语法 ==="
ck "customize.sh sh -n"  sh -n "$C"
ck "uninstall.sh sh -n"  sh -n "$U"

echo
echo "=== 主程序 profile 记录 ==="
ck "源码写 node_ 路径"   grep -q 'node_fake_temp=%s' "$REPO/src/temp_charge_boost.cpp"
ck "源码读 node_ 路径"   grep -q 'g_n_thermal_rm.empty()' "$REPO/src/temp_charge_boost.cpp"

echo
echo "SUMMARY: PASS=$PASS FAIL=$FAIL"
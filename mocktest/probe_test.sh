#!/bin/bash
# probe_test.sh — 专项验证「语义探测失败时不留污染」
#
# 背景（用户要求）：
#   "失测不成功的话，避免不要污染啥的"
#   → 探测会临时改 fake_temp，任何异常路径都必须把原值写回；
#     并且判定失败时绝不能被当成"可信真实源"。
#
# 本测试要证明三件事：
#   A. 真机布局：batt_therm 排第一，实测 1 次即命中并停止（安装不拖沓）
#   B. 无 batt_therm 时：靠实测揪出被污染节点，不会误信
#   C. 各种失败场景下：①不残留污染 ②不把失败当可信 ③日志有明确告警
#
# 关键构造技巧：
#   用符号链接让被污染节点的 temp 指向 fake_temp 文件本身，
#   这样"写 fake_temp"就等于"改这个节点的温度"，
#   精确复现真机上 battery/batt_temp 被 fake_temp 改写的语义。
#
# 注意：不能用 chmod 000 构造"不可读/不可写"——本测试以 root 运行，
#       root 无视 DAC 权限，chmod 挡不住。改用"节点指向不存在的目录"
#       与"节点是目录而非文件"来构造真实的读写失败。
set -u
BIN=${1:-/root/tcb_build/t_bin}
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  PASS  $1"; }
ng(){ FAIL=$((FAIL+1)); echo "  FAIL  $1"; }

build_env() {
  R=$1
  # 关键：上一轮测试若异常退出，会留下 bind mount。
  # bind mount 之下的目录 rm -rf 删不掉（EBUSY 语义），
  # 于是"新建环境"实际是在脏目录上叠加 → 测试结果不可信
  # （本轮 L 组就是在 fake_temp 写失败的空环境里"偶然通过"的）。
  # 因此先按挂载深度倒序全部剥掉，再删除目录。
  if [ -d "$R" ]; then
    awk -v p="$R" '$5==p || index($5,p"/")==1 {print $5}' /proc/self/mountinfo 2>/dev/null \
      | awk '{print length, $0}' | sort -rn | cut -d' ' -f2- \
      | while read -r m; do umount -l "$m" 2>/dev/null; done
    wait 2>/dev/null
  fi
  rm -rf $R 2>/dev/null
  if [ -e "$R" ]; then
    echo "  [环境异常] $R 无法清空（可能有残留挂载或权限问题），测试结果不可信"
    FAIL=$((FAIL+1))
    return 1
  fi
  mkdir -p $R/sys/devices/virtual/thermal $R/sys/class/xm_power/fuelgauge/strategy_fg
  Z=$R/sys/devices/virtual/thermal
  X=$R/sys/class/xm_power
  P=$R/sys/class/power_supply
  mkdir -p $X/charger/charger_thermal $X/charger/quick_charge
  mkdir -p $P/battery $P/usb

  echo 350       > $X/fuelgauge/strategy_fg/fake_temp
  echo 0         > $X/charger/charger_thermal/wired_ctrl_limit
  echo 1         > $X/charger/quick_charge/fake_pps_ptf
  echo 22000000  > $X/charger/charger_thermal/wired_chg_curr
  echo 350 > $P/battery/temp; echo Charging > $P/battery/status; echo 1 > $P/usb/online

  local FT=$X/fuelgauge/strategy_fg/fake_temp

  # 被污染节点：temp 是指向 fake_temp 的软链（写 fake_temp = 改该节点温度）
  # fake_temp 是 0.1℃ 量级（350 = 35.0℃），故软链节点也是 0.1℃ 量级，
  # 正好覆盖"单位非毫摄氏度"的机型（阈值必须自适应，否则会误判为可信）。
  mkdir -p $Z/thermal_zone67; echo -n "battery"   > $Z/thermal_zone67/type
  ln -sf $FT $Z/thermal_zone67/temp
  mkdir -p $Z/thermal_zone70; echo -n "batt_temp" > $Z/thermal_zone70/type
  ln -sf $FT $Z/thermal_zone70/temp

  # 独立 NTC：真实文件，不跟随（毫摄氏度 42600 = 42.6℃）
  mkdir -p $Z/thermal_zone85; echo -n "batt_therm" > $Z/thermal_zone85/type
  echo 42600 > $Z/thermal_zone85/temp

  # CPU 每核温度（真机是 cpu-N-M-* 独立 NTC，同样在伪装范围内）
  # 这些节点也必须进快照，否则 WebUI 的 cpu_list / cpu_max 全是 0
  mkdir -p $Z/thermal_zone20; echo -n "cpu-0-0-0" > $Z/thermal_zone20/type; echo 55000 > $Z/thermal_zone20/temp
  mkdir -p $Z/thermal_zone21; echo -n "cpu-0-1-0" > $Z/thermal_zone21/type; echo 52000 > $Z/thermal_zone21/temp
  mkdir -p $Z/thermal_zone22; echo -n "cpu-1-0-0" > $Z/thermal_zone22/type; echo 48000 > $Z/thermal_zone22/temp

  mkdir -p $Z/thermal_zone36; echo -n "gpuss-0" > $Z/thermal_zone36/type; echo 55000 > $Z/thermal_zone36/temp
  mkdir -p $Z/thermal_zone86; echo -n "conn_therm" > $Z/thermal_zone86/type; echo 38000 > $Z/thermal_zone86/temp
  chmod -R u+rwX $R
  export TCB_ROOT=$R
  export TCB_OS_CODE=3        # 固定走 OS3 候选顺序，避免宿主 getprop 影响结果
  D=$R/data/adb/temp_charge_boost
}

echo "############ A. 真机布局：batt_therm 优先，命中即停 ############"
build_env /tmp/tcbp1
ORIG=$(cat $X/fuelgauge/strategy_fg/fake_temp)
$BIN scan >/dev/null 2>&1
AFTER=$(cat $X/fuelgauge/strategy_fg/fake_temp)
[ "$ORIG" = "$AFTER" ] && ok "探测后 fake_temp 无残留（$ORIG → $AFTER）" || ng "fake_temp 被污染：$ORIG → $AFTER"
grep -q 'batt_type=batt_therm' $D/profile.conf && ok "选中 batt_therm 为可信源" || ng "可信源选错: $(grep batt_ $D/profile.conf | tr '\n' ' ')"
grep -q 'batt_trusted=1' $D/profile.conf && ok "batt_trusted=1（实测确认独立 NTC）" || ng "batt_trusted 不是 1"
grep -qE 'batt_fb_type=(battery|batt_temp)' $D/profile.conf && ok "污染节点归入兜底（仅显示）" || ng "兜底节点记录错"
grep -q 'zone85.*独立传感器' $D/run.log && ok "日志明确标记 zone85 独立" || ng "日志缺独立标记"
N=$(grep -c '探测：fake_temp' $D/run.log)
[ "$N" = "1" ] && ok "首候选命中即停（实测 1 次，安装不拖沓）" || ng "实测次数 $N ≠ 1"
grep -q 'batt_temp.*已知污染名' $D/run.log && ok "未实测的污染节点按已知语义归类" || ng "污染节点归类缺失"

echo ""
echo "############ B. 无 batt_therm：实测揪出污染节点，不得误信 ############"
build_env /tmp/tcbp2
rm -rf $Z/thermal_zone85          # 模拟没有独立 NTC 的机型
$BIN scan >/dev/null 2>&1
grep -q 'zone70.*跟随（被污染，不可信）' $D/run.log && ok "zone70 被实测判定为污染（单位自适应生效）" || ng "zone70 未被识别为污染: $(grep -E 'zone70|semtest' $D/run.log | tr '\n' ' ')"
grep -q 'batt_trusted=0' $D/profile.conf && ok "无独立 NTC ⇒ batt_trusted=0（不冒充可信）" || ng "无独立 NTC 却标可信"
grep -q '未找到可信电池温度源' $D/run.log && ok "明确记录「未找到可信源」" || ng "缺「未找到可信源」日志"

echo ""
echo "############ C. 失败场景：fake_temp 不可读 ############"
build_env /tmp/tcbp3
FT=$X/fuelgauge/strategy_fg/fake_temp
rm -f $FT; mkdir -p $FT       # 目录不是普通文件 ⇒ read 必然失败（root 也绕不过）
$BIN scan >/dev/null 2>&1
grep -q 'fake_temp 不可读' $D/run.log && ok "不可读时给出明确日志" || ng "不可读时无日志"
grep -q 'batt_trusted=0' $D/profile.conf && ok "无实测结果 ⇒ batt_trusted=0（不冒充可信）" || ng "不可读却标成可信"

echo ""
echo "############ D. 失败场景：节点读取失败 ############"
build_env /tmp/tcbp4
FT=$X/fuelgauge/strategy_fg/fake_temp
rm -f $Z/thermal_zone85/temp; mkdir -p $Z/thermal_zone85/temp   # 目录 ⇒ 读失败
ORIG=$(cat $FT)
$BIN scan >/dev/null 2>&1
AFTER=$(cat $FT)
[ "$ORIG" = "$AFTER" ] && ok "节点读失败时 fake_temp 依然无残留" || ng "节点读失败导致污染：$ORIG → $AFTER"
grep -q 'temp 不可读' $D/run.log && ok "节点读失败有明确日志" || ng "节点读失败无日志"

echo ""
echo "############ E. 失败场景：fake_temp 不可写 ############"
build_env /tmp/tcbp5
FT=$X/fuelgauge/strategy_fg/fake_temp
ORIG=$(cat $FT)
rm -f $FT; mkdir -p $FT/block          # 目录 ⇒ 写入失败
chmod 755 $FT
$BIN scan >/dev/null 2>&1
grep -qE '无法写 fake_temp|fake_temp 不可读' $D/run.log && ok "不可写时给出明确日志" || ng "不可写时无日志"
grep -q 'batt_trusted=0' $D/profile.conf && ok "无法探测 ⇒ batt_trusted=0" || ng "无法探测却标成可信"

echo ""
echo "############ F. 关闭探测时不得冒充可信 ############"
build_env /tmp/tcbp6
export TCB_NO_SEMTEST=1
$BIN scan >/dev/null 2>&1
unset TCB_NO_SEMTEST
grep -q 'batt_trusted=0' $D/profile.conf && ok "TCB_NO_SEMTEST ⇒ batt_trusted=0（未知不等于可信）" || ng "关闭探测却标成可信"
grep -q 'TCB_NO_SEMTEST 已设置' $D/run.log && ok "跳过探测有明确日志" || ng "跳过探测无日志"

echo ""
echo "############ G. 零残留：连续多次扫描 fake_temp 恒定 ############"
build_env /tmp/tcbp7
FT=$X/fuelgauge/strategy_fg/fake_temp
echo 366 > $FT
BAD=0
for i in 1 2 3; do
  $BIN scan >/dev/null 2>&1
  v=$(cat $FT 2>/dev/null)
  [ "$v" = "366" ] || { BAD=1; ng "第 $i 次扫描后 fake_temp=$v（应为 366）"; }
done
[ "$BAD" = "0" ] && ok "连续 3 次扫描，fake_temp 始终为 366（零残留）"

echo ""
echo "############ H. batt_trusted=0 时温度墙必须拒绝限流 ############"
build_env /tmp/tcbp8
rm -rf $Z/thermal_zone85                    # 无独立 NTC
$BIN scan >/dev/null 2>&1
grep -q 'batt_trusted=0' $D/profile.conf && ok "确认 profile: batt_trusted=0" || ng "profile 前置条件不成立"
B=$R/sys/class/backlight/panel0-backlight; mkdir -p $B; echo 16383 > $B/max_brightness; echo 1079 > $B/brightness
W=$X/charger/charger_thermal/wired_chg_curr
echo 22000000 > $W
$BIN wall 51 >/dev/null 2>&1
cat > /tmp/h_runner.sh <<'EOS'
#!/bin/bash
export TCB_ROOT=$1
shift
exec "$@"
EOS
chmod +x /tmp/h_runner.sh
timeout 6 $BIN wall on >/dev/null 2>&1 &
sleep 4
v=$(cat $W 2>/dev/null)
kill %1 2>/dev/null
[ "$v" = "22000000" ] && ok "batt_trusted=0 时温度墙拒绝限流（curr 仍满额 $v）" || ng "batt_trusted=0 却限流到 $v"
grep -q '温度墙停用：batt_trusted=0' $D/run.log && ok "停用原因写入日志" || ng "日志缺停用原因"

echo ""
echo "############ I. restore 必须恢复充电电流（卸载不留限流残留）############"
build_env /tmp/tcbp9
# 模拟温度墙正在限流：电流压到 3A
echo 3000000 > $X/charger/charger_thermal/wired_chg_curr
$BIN restore >/dev/null 2>&1
v=$(cat $X/charger/charger_thermal/wired_chg_curr 2>/dev/null)
[ "$v" = "22000000" ] && ok "restore 把限流的 3A 恢复为满额 22000000" || ng "restore 未恢复电流，仍为 $v"
v=$(cat $X/charger/charger_thermal/wired_ctrl_limit 2>/dev/null)
[ "$v" = "0" ] && ok "restore 把 wired_ctrl_limit 归 0（放开接管）" || ng "ctrl_limit 未归 0: $v"

echo ""
echo "############ J. 卸载脚本静态审计（规范合规）############"
UNINST=${UNINST:-/root/tcb_repo/uninstall.sh}
if [ -f "$UNINST" ]; then
    sh -n "$UNINST" 2>/dev/null && ok "uninstall.sh 语法通过" || ng "uninstall.sh 语法错误"
    grep -q 'MODDIR 不安全' "$UNINST" && ok "有 MODDIR 安全校验（防 rm -rf 灾难）" || ng "缺 MODDIR 安全校验"
    grep -q 'wired_chg_curr' "$UNINST" && ok "卸载会恢复充电电流" || ng "卸载不恢复充电电流（会卡限流）"
    grep -q 'fake_temp' "$UNINST" && ok "卸载会归位 fake_temp" || ng "卸载不处理 fake_temp"
    grep -q 'mountinfo' "$UNINST" && ok "用 mountinfo 判断残留挂载" || ng "缺挂载残留校验"
    grep -q '已卸载' "$UNINST" && ok "标记 module.prop 为已卸载" || ng "未标记 module.prop"
    # 安全检查：不得在未校验情况下 rm -rf 变量路径
    if grep -qE 'rm -rf "\$MODDIR"' "$UNINST"; then
        ng "存在 rm -rf \$MODDIR（危险，应交给管理器清理）"
    else
        ok "不直接 rm -rf \$MODDIR（交给管理器清理，安全）"
    fi
else
    echo "  SKIP  未找到 uninstall.sh（$UNINST）"
fi

echo ""
echo "############ K. 安装脚本静态审计（规范合规）############"
CUST=${CUST:-/root/tcb_repo/customize.sh}
if [ -f "$CUST" ]; then
    sh -n "$CUST" 2>/dev/null && ok "customize.sh 语法通过" || ng "customize.sh 语法错误"
    grep -q 'SKIPUNZIP=0' "$CUST" && ok "声明 SKIPUNZIP=0" || ng "缺 SKIPUNZIP=0"
    # 铁律：不能用 echo 做用户输出（recovery 不可见），只检测真正写 stdout 的 echo
    # 带 >> 或 > 重定向的是写日志文件，不算用户输出
    BARE=$(grep -nE '^[[:space:]]*echo ' "$CUST" | grep -v 'ui_print' | grep -vE '>>|> *"?\$' | grep -v '1>&2' || true)
    if [ -n "$BARE" ]; then
        ng "存在裸 echo 用户输出（recovery 不可见，应用 ui_print）: $(echo "$BARE" | head -2 | tr '\n' ' ')"
    else
        ok "用户输出全部走 ui_print（echo 仅用于写日志/错误流）"
    fi
    grep -q 'abort' "$CUST" && ok "错误路径用 abort 而非 exit" || ng "缺 abort"
    grep -q 'wait_for_volume_key' "$CUST" && ok "有音量键交互" || ng "无音量键交互"
    grep -q 'getevent -lqc 1' "$CUST" && ok "音量键用 -c 1 轮询（规避计数被触摸屏吃光）" || ng "音量键写法有隐患"
    grep -q 'KEY_VOLUMEDOWN.*DOWN' "$CUST" && ok "匹配「键名+DOWN」双 token（防一按算两次）" || ng "按键匹配不严"
    grep -q 'KEY_VOLUMEUP' "$CUST" && grep -q 'KEY_VOLUMEDOWN' "$CUST" && ok "上下键都可选" || ng "缺少上键或下键"
    # 末尾不得有裸 exit（规范：会跳过管理器清理）
    LAST=$(grep -vE '^[[:space:]]*(#|$)' "$CUST" | tail -1)
    case "$LAST" in
        exit*) ng "customize.sh 末尾有 exit（规范禁止）" ;;
        *)     ok "customize.sh 末尾无 exit" ;;
    esac
    grep -q 'set_perm' "$CUST" && ok "设置了文件权限" || ng "未设权限"
else
    echo "  SKIP  未找到 customize.sh（$CUST）"
fi

echo ""
echo "############ L. 时间跳变不得让真实温度读取失效 ############"
# 复现线上事故：RTC 未同步 → 系统时间跳回过去 → 快照 mtime 变成 1970
# → 旧实现算 age 得负数 → 快照全丢 → 回退读 sysfs（被伪装）→ 显示 0.3℃
build_env /tmp/tcbp10
D=$R/data/adb/temp_charge_boost
# 建立伪装（同时写快照 + meta）
$BIN apply 350 >/dev/null 2>&1
# 确认 .meta 存在（快照新鲜度判定的载体）
[ -f $D/real/.meta ] && ok "flush 写出了 .meta（boot_id + 单调时钟）" || ng "未写 .meta，快照无法判新鲜"
META=$(cat $D/real/.meta 2>/dev/null)
echo "$META" | grep -qE '^[0-9a-f-]{36} [0-9]+' && ok ".meta 格式正确（boot_id + mono_ms）" || ng ".meta 格式异常: $META"
# 把快照文件 mtime 改成 1970（模拟时钟跳变后写入）
find $D/real -name 'thermal_zone*' -exec touch -t 197001140044 {} \; 2>/dev/null
# 关键：真实温度仍必须读得出来（不能因 mtime 是 1970 就丢弃）
OUT=$($BIN dump 2>/dev/null)
V=$(echo "$OUT" | tr ',' '\n' | grep '"batt_ntc"' | grep -o '[0-9]*$')
if [ -n "$V" ] && [ "$V" != "350" ] && [ "$V" -gt 1000 ]; then
    ok "mtime=1970 时仍读到真实温度 batt_ntc=$V（不再被误判陈旧）"
else
    ng "mtime=1970 时真实温度读取失败: batt_ntc=$V（期望 >1000 且 != 350）"
fi
# CPU 同理
CPU=$(echo "$OUT" | tr ',' '\n' | grep -A0 '"cpu_max"' | grep -o '[0-9]*$')
if [ -n "$CPU" ] && [ "$CPU" != "350" ] && [ "$CPU" -gt 1000 ]; then
    ok "mtime=1970 时 CPU 温度正常 cpu_max=$CPU"
else
    ng "mtime=1970 时 CPU 温度异常: cpu_max=$CPU"
fi

echo ""
echo "############ M. 已伪装节点不得被当成真实温度源 ############"
# 快照完全不可用时（删掉 .meta），read_best 回退 sysfs；
# 但该节点已 bind mount，读到的是伪装值 350 —— 必须拒绝，返回 0 而不是 350。
rm -f $D/real/.meta
OUT2=$($BIN dump 2>/dev/null)
V2=$(echo "$OUT2" | tr ',' '\n' | grep '"batt_ntc"' | grep -o '[0-9]*$')
if [ "$V2" = "0" ] || [ -z "$V2" ]; then
    ok "无快照时拒绝把伪装值当真值（batt_ntc=$V2）"
elif [ "$V2" = "350" ]; then
    ng "无快照时把伪装值 350 当成真实温度（应为 0）"
else
    ok "无快照时仍读到有效真值 batt_ntc=$V2"
fi

echo ""
echo "############ N. real_batt_temp 不得取电荷泵陈旧值 ############"
build_env /tmp/tcbp11
D=$R/data/adb/temp_charge_boost
# 造 cp_master：batt_temp=6400（陈旧）+ ibus=0（未充电）→ 必须被排除
mkdir -p $R/sys/class/xm_power/cp_master
echo 6400 > $R/sys/class/xm_power/cp_master/batt_temp
echo 0    > $R/sys/class/xm_power/cp_master/ibus
$BIN apply 350 >/dev/null 2>&1
OUT3=$($BIN dump 2>/dev/null)
RB=$(echo "$OUT3" | tr ',' '\n' | grep '"real_batt_temp"' | grep -o '[0-9]*$')
if [ "$RB" != "6400" ]; then
    ok "ibus=0 时排除了 cp_master 陈旧值 6400（real_batt_temp=$RB）"
else
    ng "仍取到 cp_master 陈旧值 6400"
fi
# 回退目标必须是独立 NTC（不能被伪装的 charger_therm 冒充）
if [ -n "$RB" ] && [ "$RB" != "3500" ]; then
    ok "回退未落到被伪装的 charger_therm（未显示 35.0℃）"
else
    ng "回退到了伪装值 3500（35.0℃），语义错误"
fi

echo ""
echo "############ O. PID 复用不得误判 daemon 存活 ############"
build_env /tmp/tcbp12
D=$R/data/adb/temp_charge_boost
mkdir -p $D
# 把 pid 文件写成一个绝对不可能是我们进程的 PID（当前 shell 自己）
echo $$ > $D/daemon.pid
OUT4=$($BIN dump 2>/dev/null)
ALIVE=$(echo "$OUT4" | tr ',' '\n' | grep '"daemon_alive"' | grep -o 'true\|false')
[ "$ALIVE" = "false" ] && ok "PID 被别的进程复用时正确报告 daemon_alive=false" || ng "PID 复用仍误报 alive=$ALIVE"
# 清掉 pid 文件也应报 false
rm -f $D/daemon.pid
OUT5=$($BIN dump 2>/dev/null)
ALIVE2=$(echo "$OUT5" | tr ',' '\n' | grep '"daemon_alive"' | grep -o 'true\|false')
[ "$ALIVE2" = "false" ] && ok "无 pid 文件时报告 daemon_alive=false" || ng "无 pid 文件仍报 alive=$ALIVE2"

echo ""
echo "############ P. 测试自身卫生：跑完不得留挂载残留 ############"
# 本组属于"测试基础设施自检"，不是被测代码的功能。
# 理由：L/N 组曾因上一轮残留 bind mount 导致 build_env 清不掉目录，
# 于是在空环境里"偶然通过"——脏环境下的 PASS 不算验证。
KEEP=${TCB_KEEP_ENV:-0}
if [ "$KEEP" = "1" ]; then
    echo "  [跳过] TCB_KEEP_ENV=1，保留 /tmp/tcbp* 供人工排查"
else
    for r in /tmp/tcbp1 /tmp/tcbp2 /tmp/tcbp3 /tmp/tcbp4 /tmp/tcbp5 /tmp/tcbp6 \
             /tmp/tcbp7 /tmp/tcbp8 /tmp/tcbp9 /tmp/tcbp10 /tmp/tcbp11 /tmp/tcbp12; do
        [ -d "$r" ] || continue
        awk -v p="$r" '$5==p || index($5,p"/")==1 {print $5}' /proc/self/mountinfo 2>/dev/null \
          | awk '{print length, $0}' | sort -rn | cut -d' ' -f2- \
          | while read -r m; do umount -l "$m" 2>/dev/null; done
        rm -rf "$r" 2>/dev/null
    done
    # 注意不能用 grep -c：无匹配时它输出 0 且退出码为 1，
    # 若写成 $(grep -c ... || echo 0) 会得到 "0\n0"（本轮踩坑 P 组自检误报）。
    LEFT=$(awk '$5 ~ /^\/tmp\/tcbp/ {n++} END {print n+0}' /proc/self/mountinfo 2>/dev/null)
    [ "$LEFT" = "0" ] && ok "测试结束无挂载残留（/tmp/tcbp* 全部清理）" \
                      || ng "仍有 $LEFT 个 /tmp/tcbp* 挂载残留"
fi

echo ""
echo "================================"
echo "通过 $PASS / 失败 $FAIL"
[ "$FAIL" = "0" ] && exit 0 || exit 1

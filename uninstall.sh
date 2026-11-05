#!/system/bin/sh
# uninstall.sh — 温度伪装 · 充电提速 卸载脚本
#
# 规范依据：模块开发规范_雨色.md「uninstall.sh 卸载脚本规范」
#   customize-sh-interactive-patterns.md §9（LOVE 卸载模板七步）
#
# 顺序纪律（很重要，顺序错会留下残留）：
#   1) MODDIR 安全校验（防 rm -rf 指错路径）
#   2) 先停守护进程 —— 不停它会在还原之后又写回去
#   3) 还原充电电流 / 充电控制点 —— 必须在守护进程已死之后，否则立刻被覆盖
#   4) 解挂载（含懒卸载兜底 + 多层剥离）
#   5) 恢复官方伪装接口为未启用
#   6) 清运行态文件（PID/锁/配置/快照/日志）
#   7) 标记 module.prop（管理器最后一次可见状态）
#
# 本脚本必须幂等：任何一步失败都不能中断后续还原。

MODDIR=${0%/*}

# ============================================================
# 1. MODDIR 安全校验 —— 防 rm -rf 灾难
# ============================================================
case "$MODDIR" in
    ""|"/"|"/data"|"/data/adb"|"/data/adb/modules"|"."|".."|"./"|"../")
        echo "uninstall: MODDIR 不安全($MODDIR)，终止" >&2
        exit 0
        ;;
esac

BIN="$MODDIR/bin/temp_charge_boost"
# 路径可覆盖：便于离线仿真，也支持非标准放置位置
R="${TCB_ROOT:-}"
WD="${TCB_WORK:-/data/adb/temp_charge_boost}"
ZONE_ROOT="${TCB_ZONE_ROOT:-$R/sys/devices/virtual/thermal}"
ZONE_CLASS="${TCB_ZONE_CLASS:-$R/sys/class/thermal}"
FAKE_DIR="$WD/fake"
PIDF="$WD/daemon.pid"
ULOG="$WD/uninstall.log"

# 确保 WORK 目录存在：否则 log() 的 >> 重定向会静默失败，
# 整个卸载过程将没有任何记录（实测：WORK 被删后卸载静默完成，无从排查）。
mkdir -p "$WD" 2>/dev/null

log() { echo "$(date '+%m-%d %H:%M:%S') $*" >> "$ULOG" 2>/dev/null; }

# 日志写不进去时的兜底：关键结论始终打到 stderr，
# 保证「卸载了什么、有没有残留」永远有据可查。
log_fb() {
    log "$*"
    [ -s "$ULOG" ] || echo "uninstall: $*" >&2
}

log "===== 卸载开始 (MODDIR=$MODDIR) ====="

# ============================================================
# 2. 停止守护进程
#    按 cmdline 精确匹配，不用 pkill -f 模糊匹配（防误杀同名进程）
# ============================================================
if [ -f "$PIDF" ]; then
    DPID=$(cat "$PIDF" 2>/dev/null)
    case "$DPID" in
        ''|*[!0-9]*) DPID="" ;;   # 非纯数字视为无效，防 kill 误伤
    esac
    if [ -n "$DPID" ] && [ -d "/proc/$DPID" ]; then
        log "停止守护进程 pid=$DPID"
        kill "$DPID" 2>/dev/null
        i=0
        while [ $i -lt 10 ] && [ -d "/proc/$DPID" ]; do
            sleep 0.2 2>/dev/null || sleep 1
            i=$((i + 1))
        done
        if [ -d "/proc/$DPID" ]; then
            log "SIGKILL pid=$DPID"
            kill -9 "$DPID" 2>/dev/null
        fi
    else
        log "PID 文件存在但进程已不在 (pid=$DPID)"
    fi
fi

# 兜底：扫 /proc 找 cmdline 指向本模块二进制的进程
for p in /proc/[0-9]*; do
    [ -r "$p/cmdline" ] || continue
    c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
    case "$c" in
        "$BIN"*)
            pid=${p#/proc/}
            [ "$pid" = "$$" ] && continue
            log "兜底清理残留进程 pid=$pid cmd=$c"
            kill -9 "$pid" 2>/dev/null
            ;;
    esac
done

# ============================================================
# 3. 还原充电电流与充电控制点
#    原脚本缺失的关键一步：卸载时若温度墙正在限流，
#    充电电流上限会永久停在限流值（最低 3A），必须恢复满额。
# ============================================================
# 节点路径来自主程序的语义探测结果（profile.conf 的 node_* 行），
# 不再写死 xm_power 的绝对路径 —— 换 ROM / 改名后卸载同样能正确还原。
PROFILE="$WD/profile.conf"
pf_get() {
    [ -f "$PROFILE" ] || return 1
    sed -n "s/^$1=//p" "$PROFILE" 2>/dev/null | head -1
}

# 优先用 profile；profile 缺失时依次回退到已知路径与语义枚举
CUR_NODE=$(pf_get node_wired_curr)
CTRL_NODE=$(pf_get node_ctrl_limit)
PPS_NODE=$(pf_get node_pps_ptf)

# 兜底：profile 里没有（旧版本升级卸装）时按已知路径找
[ -n "$CUR_NODE" ]  || CUR_NODE=$R/sys/class/xm_power/charger/charger_thermal/wired_chg_curr
[ -n "$CTRL_NODE" ] || CTRL_NODE=$R/sys/class/xm_power/charger/charger_thermal/wired_ctrl_limit
[ -n "$PPS_NODE" ]  || PPS_NODE=$R/sys/class/xm_power/charger/quick_charge/fake_pps_ptf

# 再兜底：已知路径也不存在时，在充电体系内按名字枚举（不写死目录名）
find_chg_node() {
    _want="$1"
    for _base in "$R/sys/class/xm_power" "$R/sys/class/power_supply"; do
        [ -d "$_base" ] || continue
        _r=$(find "$_base" -maxdepth 3 -name "$_want" -type f 2>/dev/null | head -1)
        [ -n "$_r" ] && { echo "$_r"; return 0; }
    done
    return 1
}
[ -e "$CUR_NODE" ]  || CUR_NODE=$(find_chg_node wired_chg_curr)
[ -e "$CTRL_NODE" ] || CTRL_NODE=$(find_chg_node wired_ctrl_limit)
[ -e "$PPS_NODE" ]  || PPS_NODE=$(find_chg_node fake_pps_ptf)

log "充电节点: curr=${CUR_NODE:-未找到} ctrl=${CTRL_NODE:-未找到} pps=${PPS_NODE:-未找到}"
FULL_CURR=22000000

if [ -n "$CUR_NODE" ] && [ -e "$CUR_NODE" ]; then
    C=$(cat "$CUR_NODE" 2>/dev/null)
    log "卸载前充电电流上限=$C"
    echo "$FULL_CURR" > "$CUR_NODE" 2>/dev/null
    C2=$(cat "$CUR_NODE" 2>/dev/null)
    if [ "$C2" = "$FULL_CURR" ]; then
        log "充电电流已恢复满额 $FULL_CURR"
    else
        log "警告：充电电流恢复失败，当前=$C2（期望 $FULL_CURR）"
    fi
fi

# wired_ctrl_limit 归 0（模块运行时会写 0 放开控制）
if [ -n "$CTRL_NODE" ] && [ -e "$CTRL_NODE" ]; then
    echo 0 > "$CTRL_NODE" 2>/dev/null && log "wired_ctrl_limit 归 0"
fi
# fake_pps_ptf 归 1（模块运行时会写 1）
if [ -n "$PPS_NODE" ] && [ -e "$PPS_NODE" ]; then
    echo 1 > "$PPS_NODE" 2>/dev/null && log "fake_pps_ptf 归 1"
fi

# ============================================================
# 4. 解挂载 —— 剥到 0 层
#    sysfs 上重复 bind 会让同一目标出现多条挂载记录，
#    mountpoint -q 只能判断"有没有挂载"，剥不干净，必须循环。
# ============================================================
strip_zone() {
    t="$1"
    n=0
    while [ $n -lt 512 ]; do
        grep -qF " $t " /proc/self/mountinfo 2>/dev/null || break
        umount "$t" 2>/dev/null || umount -l "$t" 2>/dev/null || break
        n=$((n + 1))
    done
    [ $n -gt 0 ] && log "剥离 $t 共 $n 层"
    return 0
}

# 按主路径与别名路径都扫（Android 上 /sys/class/thermal 是软链，两条都要试）
for base in "$ZONE_ROOT" "$ZONE_CLASS"; do
    [ -d "$base" ] || continue
    for z in "$base"/thermal_zone*; do
        [ -d "$z" ] || continue
        strip_zone "$z/temp"
    done
done

# 兜底：直接扫 mountinfo，把源路径落在本模块假值目录下的挂载全部卸掉。
# 这一步不依赖热区枚举是否成功 —— 即便 ROM 换了目录结构、或者模块曾把
# 假值挂到了别的文件上，只要还挂着就一定在这里出现。
if [ -d "$FAKE_DIR" ] || [ -n "$TCB_WORK" ]; then
    # 兜底：扫 mountinfo 把属于本模块伪装的挂载全部卸掉，不依赖热区目录是否存在。
    #
    # 判据（双路）：
    #   A. 字段 4（root）以 $FAKE_DIR 开头 —— 真实 Linux 上 bind mount 的 root 就是源路径
    #   B. "-" 之后的 source 字段以 $FAKE_DIR 开头 —— proot/容器下 root 恒为 '/'，只能靠这里
    # 实测样本（proot）：1000000 1 0:1 / /tmp/mm/b/temp rw - bind /data/.../tmp/mm/a/temp rw
    # 只写 A 会让兜底永远空转（v1.4.3 的 index(root,src)==1 就是这个 bug，从未生效过）。
    awk -v src="$FAKE_DIR" '
        {
            sep = 0
            for (i = 1; i <= NF; i++) if ($i == "-") { sep = i; break }
            if (sep == 0) next
            mnt = $5
            hit = 0
            if (index($4, src) == 1 && ($4 == src || substr($4, length(src) + 1, 1) == "/")) hit = 1
            if (!hit && (sep + 2) <= NF) {
                s = $(sep + 2)
                if (index(s, src) == 1 && (s == src || substr(s, length(src) + 1, 1) == "/")) hit = 1
            }
            if (hit) print mnt
        }
    ' /proc/self/mountinfo 2>/dev/null | sort -u | while read -r m; do
        [ -n "$m" ] || continue
        log "兜底卸载残留挂载: $m"
        strip_zone "$m"
    done
fi

# 校验：还有没有残留挂载指向本模块假值目录。
# 判据一：按假值目录的真实路径（支持 WORK 被改过）；
# 判据二：按固定模块名（兼容旧版本残留）。
LEFT=$(grep -cF "$FAKE_DIR/" /proc/self/mountinfo 2>/dev/null)
case "$LEFT" in ''|*[!0-9]*) LEFT=0 ;; esac
LEFT2=$(grep -c "temp_charge_boost/fake" /proc/self/mountinfo 2>/dev/null)
case "$LEFT2" in ''|*[!0-9]*) LEFT2=0 ;; esac
[ "$LEFT2" -gt "$LEFT" ] && LEFT=$LEFT2
case "$LEFT" in ''|*[!0-9]*) LEFT=0 ;; esac
if [ "$LEFT" -gt 0 ]; then
    log_fb "警告：仍有 $LEFT 处挂载残留（可能被其他命名空间持有），已尝试懒卸载"
else
    log_fb "挂载已全部解除"
fi

# ============================================================
# 5. 恢复官方伪装接口为未启用（-999 = 关闭）
# ============================================================
FAKE=$(pf_get node_fake_temp)
[ -n "$FAKE" ] || FAKE=$R/sys/class/xm_power/fuelgauge/strategy_fg/fake_temp
[ -e "$FAKE" ] || FAKE=$(find_chg_node fake_temp)
if [ -n "$FAKE" ] && [ -e "$FAKE" ]; then
    echo -999 > "$FAKE" 2>/dev/null
    log "fake_temp 归 -999（未启用）"
fi

# ============================================================
# 6. 清运行态（列全清单，逐个确认）
# ============================================================
rm -f "$PIDF" 2>/dev/null
rm -f "$WD/wall.conf"    2>/dev/null
rm -f "$WD/target.conf"  2>/dev/null
rm -f "$WD/profile.conf" 2>/dev/null
rm -f "$WD/install.log"  2>/dev/null
rm -rf "$WD/fake"        2>/dev/null
rm -rf "$WD/real"        2>/dev/null

log "运行态文件已清理"
# 日志保留：卸载是最危险的环节，出问题必须能事后复盘。
# 目录仅几 KB，不构成残留负担；下次安装会覆盖。
# rmdir 只在空目录时成功；非空说明还有未知文件，保留目录便于排查
# 保留 $WD（日志在其中），仅提示位置
log_fb "卸载日志保留于 $ULOG"

# ============================================================
# 7. 标记 module.prop（管理器最后一次可见状态要干净）
# ============================================================
MP="$MODDIR/module.prop"
if [ -f "$MP" ]; then
    if ! grep -q "已卸载" "$MP" 2>/dev/null; then
        sed -i 's|^description=.*|description=已卸载。温度伪装与充电控制均已还原。|' "$MP" 2>/dev/null
    fi
fi

# 末尾不写 exit，交给管理器清理 MODPATH
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
    ""|"/"|"/data"|"/data/adb"|"/data/adb/modules"|"."|"*")
        echo "uninstall: MODDIR 不安全($MODDIR)，终止" >&2
        exit 0
        ;;
esac

BIN="$MODDIR/bin/temp_charge_boost"
WD=/data/adb/temp_charge_boost
PIDF="$WD/daemon.pid"
ULOG="$WD/uninstall.log"

log() { echo "$(date '+%m-%d %H:%M:%S') $*" >> "$ULOG" 2>/dev/null; }

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
CUR_NODE=/sys/class/xm_power/charger/charger_thermal/wired_chg_curr
CTRL_NODE=/sys/class/xm_power/charger/charger_thermal/wired_ctrl_limit
PPS_NODE=/sys/class/xm_power/charger/quick_charge/fake_pps_ptf
FULL_CURR=22000000

if [ -e "$CUR_NODE" ]; then
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
if [ -e "$CTRL_NODE" ]; then
    echo 0 > "$CTRL_NODE" 2>/dev/null && log "wired_ctrl_limit 归 0"
fi
# fake_pps_ptf 归 1（模块运行时会写 1）
if [ -e "$PPS_NODE" ]; then
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

# 两个别名路径都扫，避免漏网
for z in /sys/devices/virtual/thermal/thermal_zone*; do
    [ -d "$z" ] || continue
    strip_zone "$z/temp"
done
for z in /sys/class/thermal/thermal_zone*; do
    [ -d "$z" ] || continue
    strip_zone "$z/temp"
done

# 校验：还有没有残留挂载指向本模块假值目录
LEFT=$(grep -c "temp_charge_boost/fake" /proc/self/mountinfo 2>/dev/null)
case "$LEFT" in ''|*[!0-9]*) LEFT=0 ;; esac
if [ "$LEFT" -gt 0 ]; then
    log "警告：仍有 $LEFT 处挂载残留（可能被其他命名空间持有），已尝试懒卸载"
else
    log "挂载已全部解除"
fi

# ============================================================
# 5. 恢复官方伪装接口为未启用（-999 = 关闭）
# ============================================================
FAKE=/sys/class/xm_power/fuelgauge/strategy_fg/fake_temp
if [ -e "$FAKE" ]; then
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
rm -f "$ULOG" 2>/dev/null
# rmdir 只在空目录时成功；非空说明还有未知文件，保留目录便于排查
rmdir "$WD" 2>/dev/null

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
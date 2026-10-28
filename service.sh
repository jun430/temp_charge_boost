#!/system/bin/sh
# service.sh — late_start，启动守护进程
# 负责持续保持充电限流档为 0、fake_temp 不被驱动重置
MODDIR=${0%/*}
BIN="$MODDIR/bin/temp_charge_boost"
WD=/data/adb/temp_charge_boost
PIDF="$WD/daemon.pid"
LOG="$WD/service.log"

log() { echo "$(date '+%m-%d %H:%M:%S') $*" >> "$LOG" 2>/dev/null; }

mkdir -p "$WD" 2>/dev/null

# 等系统稳定（boot_completed 比固定 sleep 更可靠；
# 但也不能无限等，超时兜底继续，否则某些机型永远起不来）
i=0
while [ $i -lt 60 ]; do
  [ "$(getprop sys.boot_completed 2>/dev/null)" = "1" ] && break
  sleep 2
  i=$((i+1))
done

# 禁用/卸载标记：管理器加了这些文件就不该继续跑
[ -f "$MODDIR/disable" ] && { log "模块已禁用，不启动"; exit 0; }
[ -f "$MODDIR/remove" ]  && { log "模块已卸载，不启动"; exit 0; }

[ -x "$BIN" ] || { log "二进制不可执行: $BIN"; exit 0; }

# 防止重复启动：PID 文件 + 进程存活双重校验
if [ -f "$PIDF" ]; then
  OLDPID=$(cat "$PIDF" 2>/dev/null)
  case "$OLDPID" in ''|*[!0-9]*) OLDPID="" ;; esac
  if [ -n "$OLDPID" ] && [ -d "/proc/$OLDPID" ]; then
    log "守护进程已在运行 pid=$OLDPID"
    exit 0
  fi
fi

# 兜底：PID 文件丢了但进程还在（例如被手动 kill -9 后残留），按 cmdline 找
for p in /proc/[0-9]*; do
  [ -r "$p/cmdline" ] || continue
  c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
  case "$c" in
    "$BIN daemon"*) log "发现已在运行的守护进程 pid=${p#/proc/}，跳过启动"; exit 0 ;;
  esac
done

log "启动守护进程"
# 后台启动（脱离当前会话，避免被杀）
setsid "$BIN" daemon >/dev/null 2>&1 &
# 启动日志裁剪：避免无限增长（>64KB 截半）
if [ -f "$LOG" ]; then
  SZ=$(wc -c < "$LOG" 2>/dev/null)
  case "$SZ" in ''|*[!0-9]*) SZ=0 ;; esac
  if [ "$SZ" -gt 65536 ]; then
    tail -c 32768 "$LOG" > "$LOG.tmp" 2>/dev/null && mv "$LOG.tmp" "$LOG" 2>/dev/null
  fi
fi

exit 0
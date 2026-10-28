#!/system/bin/sh
# service.sh — late_start，启动守护进程
# 负责持续保持充电限流档为 0、fake_temp 不被驱动重置

MODDIR=${0%/*}
BIN="$MODDIR/bin/temp_charge_boost"
PIDF=/data/adb/temp_charge_boost/daemon.pid

# 等系统稳定
sleep 20

[ -x "$BIN" ] || exit 0

# 防止重复启动
if [ -f "$PIDF" ]; then
  OLDPID=$(cat "$PIDF" 2>/dev/null)
  if [ -n "$OLDPID" ] && [ -d "/proc/$OLDPID" ]; then
    exit 0
  fi
fi

# 后台启动（脱离当前会话，避免被杀）
setsid "$BIN" daemon >/dev/null 2>&1 &

exit 0
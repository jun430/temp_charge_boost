#!/system/bin/sh
# uninstall.sh — 卸载时完整恢复
# 必须确保所有温度伪装全部解除，fake_temp 归位

MODDIR=${0%/*}
BIN="$MODDIR/bin/temp_charge_boost"
PIDF=/data/adb/temp_charge_boost/daemon.pid

# 1. 停止守护进程
if [ -f "$PIDF" ]; then
  DPID=$(cat "$PIDF" 2>/dev/null)
  [ -n "$DPID" ] && kill "$DPID" 2>/dev/null
  sleep 1
  [ -n "$DPID" ] && kill -9 "$DPID" 2>/dev/null
fi

# 2. 用二进制恢复（最完整）
if [ -x "$BIN" ]; then
  "$BIN" restore 2>/dev/null
fi

# 3. 双保险：扫描所有 thermal_zone 强制剥离所有叠层
#    注意：sysfs 上重复 bind 会导致同一目标出现多条挂载记录，
#    mountpoint -q 只判断"是否有挂载"，无法剥离多层，这里用循环剥离。
strip_zone() {
  t="$1"
  n=0
  while [ $n -lt 4096 ]; do
    # 该挂载点已无记录则退出
    if ! grep -qF " $t " /proc/self/mountinfo 2>/dev/null; then
      break
    fi
    umount "$t" 2>/dev/null || umount -l "$t" 2>/dev/null || break
    n=$((n + 1))
  done
}
for z in /sys/devices/virtual/thermal/thermal_zone*; do
  [ -d "$z" ] || continue
  strip_zone "$z/temp"
done

# 4. 兜底再扫一次（防止别名路径漏网）
for z in /sys/class/thermal/thermal_zone*; do
  [ -d "$z" ] || continue
  strip_zone "$z/temp"
done

# 5. 恢复官方温度伪装接口为未启用
echo -999 > /sys/class/xm_power/fuelgauge/strategy_fg/fake_temp 2>/dev/null

# 6. 清理工作目录
rm -rf /data/adb/temp_charge_boost 2>/dev/null

exit 0
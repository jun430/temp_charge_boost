#!/system/bin/sh
# post-fs-data.sh — 开机早期，建立温度伪装挂载
# 此时 /data 已挂载，sysfs 已就绪

MODDIR=${0%/*}
BIN="$MODDIR/bin/temp_charge_boost"

# 等待 sysfs 就绪（最多 15 秒）
i=0
while [ $i -lt 15 ]; do
  [ -d /sys/devices/virtual/thermal/thermal_zone0 ] && break
  sleep 1
  i=$((i+1))
done

[ -x "$BIN" ] || exit 0

# 应用伪装（幂等）
"$BIN" apply 350 2>/dev/null

exit 0
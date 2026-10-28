#!/system/bin/sh
# customize.sh — 温度伪装 · 充电提速 安装脚本
#
# 规范依据：
#   refs/magisk-guides.md     customize.sh 变量/函数/铁律
#   customize-sh-interactive-patterns.md   音量键轮询架构
#   模块开发规范_雨色.md      门禁检查 + ui_print + 作者署名
#
# 铁律（照抄规范，勿改）：
#   · 不用 echo 输出（recovery 下不可见），一律 ui_print
#   · 不用 exit 结束（会跳过管理器清理），出错用 abort
#   · 不在 zip 放 install.sh、不改 update-binary
#   · 末尾不写 exit
SKIPUNZIP=0
ASH_STANDALONE=1

# 安装日志放工作目录而不是 $MODPATH：
# abort 时管理器会清理 MODPATH，日志若在里面就一起没了，失败反而无从排查。
WD=/data/adb/temp_charge_boost
mkdir -p "$WD" 2>/dev/null
INSTALL_LOG="$WD/install.log"
: > "$INSTALL_LOG" 2>/dev/null

# ---------- 日志（安装期全程记录，便于排查） ----------
log_info()  { echo "INFO  $*" >> "$INSTALL_LOG" 2>/dev/null; }
log_ok()    { echo "OK    $*" >> "$INSTALL_LOG" 2>/dev/null; }
log_warn()  { echo "WARN  $*" >> "$INSTALL_LOG" 2>/dev/null; }
log_error() { echo "ERROR $*" >> "$INSTALL_LOG" 2>/dev/null; }

# ---------- 音量键读取 ----------
# 为什么这样写（踩坑对照，见 customize-sh-interactive-patterns.md §1/§2）：
#   1) 不显式列 /dev/input/eventN —— 交给内核广播，音量键在哪个 event 都能收到
#   2) 每次只用 -c 1 —— 避免触摸屏 EV_ABS 事件把计数吃光导致按键永远轮不到
#   3) 在循环里反复重开 —— 按键发生在两次抓取之间也不会漏
#   4) 匹配 "键名 + DOWN" 双 token —— 只认按下，释放(UP)不触发，防一按算两次
# 返回约定：1=音量上  2=音量下  3=超时
wait_for_volume_key() {
    local timeout=${1:-15} start_time=$(date +%s)
    while true; do
        [ $(( $(date +%s) - start_time )) -ge $timeout ] && { echo "3"; return; }
        local event=$(getevent -lqc 1 2>/dev/null | {
            while read -r line; do
                case "$line" in
                    *KEY_VOLUMEDOWN*DOWN*) echo "2" && break ;;
                    *KEY_VOLUMEUP*DOWN*)   echo "1" && break ;;
                esac
            done
        })
        [ -n "$event" ] && { echo "$event"; return; }
        if command -v usleep >/dev/null 2>&1; then usleep 50000; else sleep 0.05; fi
    done
}

# 三行固定格式的选项文案
opt() {
    ui_print "  [+] 音量+ : $1"
    ui_print "  [-] 音量- : $2"
    ui_print "  [ ] 等待  : $3"
}

ui_print "━━━━━━━━━━━━━━━━━━━"
ui_print " 温度伪装 · 充电提速"
ui_print " 作者: 雨色"
ui_print "━━━━━━━━━━━━━━━━━━━"
ui_print " "

log_info "===== 安装开始 ====="
log_info "MODPATH=$MODPATH BOOTMODE=$BOOTMODE ARCH=$ARCH API=$API"
log_info "MAGISK_VER=$MAGISK_VER($MAGISK_VER_CODE) KSU_VER=$KSU_VER APATCH=$APATCH"

# ============================================================
# 门禁 1：架构
# ============================================================
if [ "$ARCH" != "arm64" ]; then
    ui_print "× 不支持的架构：$ARCH"
    log_error "ARCH=$ARCH，需要 arm64"
    abort "本模块的二进制为 arm64-v8a，仅支持 64 位设备。"
fi
log_ok "架构检查通过: $ARCH"

# ============================================================
# 门禁 2：必需节点
# ============================================================
if [ ! -d /sys/devices/virtual/thermal ]; then
    ui_print "× 找不到 thermal 子系统"
    log_error "/sys/devices/virtual/thermal 不存在"
    abort "本机没有 thermal_zone，温度伪装无法工作。"
fi
if [ ! -e /sys/class/xm_power/fuelgauge/strategy_fg/fake_temp ]; then
    ui_print "× 找不到小米充电官方伪装接口"
    ui_print "  /sys/class/xm_power/fuelgauge/strategy_fg/fake_temp"
    log_error "fake_temp 不存在"
    abort "本模块依赖澎湃 OS 的 xm_power 充电接口，当前系统不受支持。"
fi
log_ok "必需节点检查通过"

# ============================================================
# 门禁 3：二进制可执行
# ============================================================
BIN="$MODPATH/bin/temp_charge_boost"
if [ ! -f "$BIN" ]; then
    ui_print "× 安装包内缺少二进制"
    log_error "缺少 $BIN"
    abort "安装包损坏，请重新下载。"
fi
chmod 0755 "$BIN" 2>/dev/null
log_ok "二进制就位: $(ls -l "$BIN" 2>/dev/null | awk '{print $5" bytes"}')"

# ============================================================
# 兼容性提示（不阻止安装）
# ============================================================
OS_CODE=$(getprop ro.mi.os.version.code)
OS_NAME=$(getprop ro.mi.os.version.name)
ui_print "- 系统: ${OS_NAME:-未知} (code=${OS_CODE:-?}) Android $(getprop ro.build.version.release)"
log_info "系统: $OS_NAME code=$OS_CODE Android $(getprop ro.build.version.release)"
if [ -z "$OS_CODE" ]; then
    ui_print "- 非澎湃 OS：将使用通用节点探测"
    log_warn "未识别澎湃版本，走通用候选顺序"
fi

# ============================================================
# 交互 1：安装模式（只判断，不写盘）
# ============================================================
ui_print " "
ui_print "━━━━━━━━━━━━━━━━━━━"
ui_print " 选择安装模式"
ui_print "━━━━━━━━━━━━━━━━━━━"
opt "标准安装（温度墙 51℃ 开启）" "轻量安装（只伪装温度，温度墙关闭）" "15 秒后默认标准安装"
MODE_KEY=$(wait_for_volume_key 15)
case "$MODE_KEY" in
    1) INSTALL_MODE="full";   ui_print "  → 标准安装"; log_ok "用户选择: 标准安装" ;;
    2) INSTALL_MODE="light";  ui_print "  → 轻量安装（温度墙默认关闭）"; log_ok "用户选择: 轻量安装" ;;
    *) INSTALL_MODE="full";   ui_print "  → 超时，按标准安装继续"; log_warn "交互超时，默认标准安装" ;;
esac

# ============================================================
# 交互 2：确认安装
# ============================================================
ui_print " "
ui_print "━━━━━━━━━━━━━━━━━━━"
ui_print " 确认安装？"
ui_print "━━━━━━━━━━━━━━━━━━━"
opt "确认安装" "取消安装" "15 秒后自动确认"
CONFIRM_KEY=$(wait_for_volume_key 15)
case "$CONFIRM_KEY" in
    2)
        ui_print "  → 已取消，未做任何改动"
        log_warn "用户取消安装"
        abort "用户取消安装。"
        ;;
    *)
        ui_print "  → 继续安装"
        ;;
esac

# ============================================================
# 部署文件
# ============================================================
ui_print " "
ui_print "- 正在部署文件..."

# 权限：脚本 0755，配置 0644
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$BIN" 0 0 0755

# ============================================================
# 生成默认配置
# ============================================================
# 温度墙配置：enabled threshold(C) force_trust
#   标准安装 → 开启，51℃
#   轻量安装 → 关闭，51℃（用户可在 WebUI 随时开）
if [ "$INSTALL_MODE" = "light" ]; then
    WALL_EN=0
else
    WALL_EN=1
fi
printf '%s %s 0\n' "$WALL_EN" "51000" > "$WD/wall.conf" 2>/dev/null
ui_print "- 温度墙配置: $([ "$WALL_EN" = "1" ] && echo "开启 51℃" || echo "关闭")"
log_ok "wall.conf: enabled=$WALL_EN threshold=51000"

# 伪装目标温度（35.0℃）
printf '350\n' > "$WD/target.conf" 2>/dev/null

# ============================================================
# 首次扫描（生成 profile.conf，行为与 apply 自动扫描一致）
# ============================================================
ui_print "- 正在扫描温度节点并判定语义..."
ui_print "  （会短暂读取 fake_temp，测完自动还原，不残留）"
log_info "执行首次 scan"

# 注意：此处在安装阶段（recovery/管理器环境），sysfs 可读。
# scan 只读 + 短暂写 fake_temp 后恢复，不改伪装状态。
SCAN_OUT=$("$BIN" scan 2>&1)
SCAN_RC=$?
echo "$SCAN_OUT" >> "$INSTALL_LOG" 2>/dev/null

if [ -f "$WD/profile.conf" ]; then
    BATT_TYPE=$(grep '^batt_type=' "$WD/profile.conf" 2>/dev/null | cut -d= -f2)
    BATT_TRUSTED=$(grep '^batt_trusted=' "$WD/profile.conf" 2>/dev/null | cut -d= -f2)
    ZONE_TOTAL=$(grep '^zone_total=' "$WD/profile.conf" 2>/dev/null | cut -d= -f2)
    MASK_CNT=$(grep '^mask_count=' "$WD/profile.conf" 2>/dev/null | cut -d= -f2)
    SKIP_CNT=$(grep '^skip_count=' "$WD/profile.conf" 2>/dev/null | cut -d= -f2)
    ui_print "  温度节点: 共 ${ZONE_TOTAL:-?} 个，伪装 ${MASK_CNT:-?} 个，跳过 ${SKIP_CNT:-?} 个"
    if [ "$BATT_TRUSTED" = "1" ]; then
        ui_print "  电池温度源: ${BATT_TYPE:-?}（实测独立 NTC，可信）"
        log_ok "电池源=$BATT_TYPE trusted=1"
    else
        ui_print "  电池温度源: 未找到独立 NTC"
        ui_print "  ⚠ 温度墙将停用（无机型可信温度，不拿假值控温）"
        log_warn "batt_trusted=0，温度墙将停用"
    fi
else
    ui_print "  ⚠ 扫描未生成 profile，将在首次开机时自动重试"
    log_error "scan 未产出 profile.conf，rc=$SCAN_RC"
fi

# ============================================================
# 完成
# ============================================================
ui_print " "
ui_print "━━━━━━━━━━━━━━━━━━━"
ui_print " 安装完成"
ui_print "━━━━━━━━━━━━━━━━━━━"
ui_print "- 开机后自动建立伪装并启动守护进程"
ui_print "- WebUI: 管理器内点开模块即可查看温度/调阈值/看日志"
ui_print "- 卸载会自动还原全部节点与充电电流"
if [ -f "$INSTALL_LOG" ]; then
    ui_print " "
    ui_print "- 安装日志: $INSTALL_LOG"
fi
log_ok "===== 安装完成 ====="

# 规范：末尾不写 exit

// temp_charge_boost.cpp
// 温度伪装 + 充电提速 守护进程
// 设备: 红米 K100 Pro Max (M098FE / songyuan) 内核 6.12.69-android16-6
// 作者: 雨色
//
// 设计要点（低开销）:
//   1. 温度伪装用 bind mount，一次性建立，运行期零开销
//   2. 充电守护单进程常驻，仅在充电时轮询，间隔 5s，未充电时 30s
//   3. 单线程 epoll-less 纯 sleep，无 fork，内存 < 1MB
//   4. 只读取必要的 sysfs，避免全盘扫描
//
// 编译: g++ -O2 -s -static -o temp_charge_boost temp_charge_boost.cpp
//
// 用法:
//   temp_charge_boost daemon   # 常驻守护（模块用）
//   temp_charge_boost apply    # 应用一次
//   temp_charge_boost restore  # 全部恢复
//   temp_charge_boost status   # 查看状态

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <cerrno>
#include <string>
#include <vector>
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctime>

// ============ 配置 ============
//
// 路径全部可被环境变量覆盖（TCB_ROOT 为总根），用途只有两个：
//   1) 离线模拟测试：把 TCB_ROOT 指向伪 sysfs 目录，就能在 PC 上跑完整逻辑
//   2) 不同机型 sysfs 位置有差异时，不用重编译
// 生产环境（KernelSU 启动）不设这些变量，走下面默认值。

static std::string g_root;          // 根前缀，空串表示真实路径
static std::string g_work;
static std::string g_fake;
static std::string g_real;
static std::string g_log;

static std::string g_n_fake_temp;
static std::string g_n_ctrl_limit;
static std::string g_n_pps_ptf;
static std::string g_n_batt_temp;
static std::string g_n_usb_online;
static std::string g_n_batt_stat;
static std::string g_n_wired_curr;
static std::string g_n_thermal_rm;   // OS4: wired_thermal_remove（充电温度限制解除标志）
static std::string g_zone_root;
static std::string g_zone_class;

static std::string env_or(const char *k, const std::string &def) {
    const char *v = getenv(k);
    return (v && *v) ? std::string(v) : def;
}

// 把所有路径初始化好。必须在 main 最开头调用一次。
static void init_paths() {
    // TCB_ROOT 只影响 sysfs 那部分，工作目录单独用 TCB_WORK 覆盖
    g_root = env_or("TCB_ROOT", "");
    std::string R = g_root;

    g_work = env_or("TCB_WORK", R + "/data/adb/temp_charge_boost");
    g_fake = g_work + "/fake";
    g_real = g_work + "/real";
    g_log  = g_work + "/run.log";

    g_zone_root  = env_or("TCB_ZONE_ROOT",  R + "/sys/devices/virtual/thermal");
    g_zone_class = env_or("TCB_ZONE_CLASS", R + "/sys/class/thermal");

    g_n_fake_temp  = env_or("TCB_FAKE_TEMP",  R + "/sys/class/xm_power/fuelgauge/strategy_fg/fake_temp");
    g_n_ctrl_limit = env_or("TCB_CTRL_LIMIT", R + "/sys/class/xm_power/charger/charger_thermal/wired_ctrl_limit");
    g_n_pps_ptf    = env_or("TCB_PPS_PTF",    R + "/sys/class/xm_power/charger/quick_charge/fake_pps_ptf");
    g_n_batt_temp  = env_or("TCB_BATT_TEMP",  R + "/sys/class/power_supply/battery/temp");
    g_n_usb_online = env_or("TCB_USB_ONLINE", R + "/sys/class/power_supply/usb/online");
    g_n_batt_stat  = env_or("TCB_BATT_STAT",  R + "/sys/class/power_supply/battery/status");
    g_n_wired_curr = env_or("TCB_WIRED_CURR", R + "/sys/class/xm_power/charger/charger_thermal/wired_chg_curr");
    // OS4 新增：内核充电驱动的温度限制解除标志。
    // 为什么需要它：实测 OS4 上 charge_interface/ichg_limit 显示
    //   buck eff_client:volt_thermal_limit 3000
    // 即内核因温度把电流卡在 3A，此时把 wired_chg_curr 抬到 18A/22A 也上不去。
    // 写 wired_thermal_remove=1 后实测电流从 5.33A → 6.07A（+0.74A），
    // 说明这是与 wired_chg_curr 并列的第二层限制，必须一并处理。
    // OS3 无此节点（node_writable 返回 false，profile.thermal_ok=0，自动跳过）。
    g_n_thermal_rm = env_or("TCB_THERMAL_RM", R + "/sys/class/xm_power/charger/charger_thermal/wired_thermal_remove");
}

#define WORK_DIR     (g_work.c_str())
#define FAKE_DIR     (g_fake.c_str())
#define REAL_DIR     (g_real.c_str())
#define LOG_FILE     (g_log.c_str())
#define ZONE_ROOT    (g_zone_root.c_str())
#define ZONE_CLASS   (g_zone_class.c_str())
#define N_FAKE_TEMP  (g_n_fake_temp.c_str())
#define N_CTRL_LIMIT (g_n_ctrl_limit.c_str())
#define N_PPS_PTF    (g_n_pps_ptf.c_str())
#define N_BATT_TEMP  (g_n_batt_temp.c_str())
#define N_USB_ONLINE (g_n_usb_online.c_str())
#define N_BATT_STAT  (g_n_batt_stat.c_str())
#define N_WIRED_CURR (g_n_wired_curr.c_str())
#define N_THERMAL_RM (g_n_thermal_rm.c_str())

// OS4 实测：wired_chg_curr 抬到 18A 后电流饱和（20A/22A 反而回落），
// 因此满额取 18A 而非 22A —— 22A 是寄存器允许值，但不是有效上限。
static const int  DEF_WIRED_CURR = 18000000;  // 默认满额 18A（OS4 实测饱和点）
static const int  WALL_CAP_CURR  = 3000000;   // 温度墙触顶时压到 3A

static const int  DEF_TEMP      = 350;   // 35.0C
static const int  POLL_CHARGING = 1;     // 充电时轮询秒（温度墙需要快速响应）
static const int  POLL_IDLE     = 10;    // 空闲轮询秒
static const int  FLUSH_INTERVAL = 3;    // 真实温度快照刷新间隔（秒），与轮询解耦

// 排除的 zone 类型（精确匹配）
// 基带 / 射频节点真实温度可达 83~89℃，伪装后系统读不到，风险过高，
// 一律保留真实值，让厂商温控仍能对基带降频。
static const char *EXCLUDE_TYPE[] = {
    "sdr0_pa",      // 射频无效传感器（恒 -273）
    "mdmss-0", "mdmss-1", "mdmss-2", "mdmss-3",   // 调制解调器
    "pa_therm1", "pa_therm2",                      // 功放
    "sdr0", "mmw_ific0",                           // 射频收发
    nullptr
};

// 排除的 zone 类型（子串匹配，用于带后缀的同类节点）
static const char *EXCLUDE_SUBSTR[] = {
    "mdmss",        // 覆盖 mdmss-N 及未来变体
    nullptr
};

// ============ 工具函数 ============

static bool read_int(const char *path, int *out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = 0;
    *out = atoi(buf);
    return true;
}

static bool read_str(const char *path, std::string &out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = 0;
    out.assign(buf);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return true;
}

static bool write_int(const char *path, int v) {
    // 真实 sysfs 会整体替换内容，但普通文件（离线模拟测试、某些 vendor 节点）
    // 不截断会导致新值拼接旧值（实测 350 写进 "3509" 变 "3509"）。
    // 加 O_TRUNC 对 sysfs 无副作用，对普通文件是必要修正。
    int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d", v);
    ssize_t w = write(fd, buf, n);
    close(fd);
    return w == n;
}

// 统计某挂载点当前叠了几层（读 /proc/self/mountinfo 第 5 字段）
// 关键：sysfs 上对同一目标重复 bind mount 会导致层数翻倍（1→2→4→8），
// 必须精确知道层数，剥到 0 才能再挂，否则会指数膨胀。
static int count_layers(const char *target) {
    FILE *fp = fopen("/proc/self/mountinfo", "r");
    if (!fp) return -1;
    char line[4096];
    int n = 0;
    while (fgets(line, sizeof(line), fp)) {
        int f = 0;
        char *p = line;
        while (*p && f < 4) { if (*p == ' ') f++; p++; }
        if (f != 4) continue;
        char *end = strchr(p, ' ');
        if (!end) continue;
        *end = 0;
        if (strcmp(p, target) == 0) n++;
    }
    fclose(fp);
    return n;
}

// 一次性扫描 mountinfo，统计所有 thermal_zone 的挂载总层数
// 优化点：只 open + 解析一遍（约 300 行），避免对 97 个 zone 各扫一遍
// （原来是 97 次 fopen/fgets/fclose，单次 dump 要 150~250ms）
static int count_all_thermal_layers() {
    FILE *fp = fopen("/proc/self/mountinfo", "r");
    if (!fp) return -1;
    char line[4096];
    int total = 0;
    const size_t pre = strlen(ZONE_ROOT);
    while (fgets(line, sizeof(line), fp)) {
        int f = 0;
        char *p = line;
        while (*p && f < 4) { if (*p == ' ') f++; p++; }
        if (f != 4) continue;
        char *end = strchr(p, ' ');
        if (!end) continue;
        *end = 0;
        // p 形如 /sys/devices/virtual/thermal/thermal_zoneN/temp
        if (strncmp(p, ZONE_ROOT, pre) != 0) continue;
        if (strstr(p, "/thermal_zone") && strstr(p, "/temp")) total++;
    }
    fclose(fp);
    return total;
}

// 把某挂载点剥到 0 层；返回实际剥离次数，失败返回 -1
static int unmount_all(const char *target) {
    int n = 0;
    while (n < 4096) {
        if (count_layers(target) == 0) break;
        if (umount(target) != 0) {
            // 普通 umount 失败时尝试懒卸载（MNT_DETACH），再失败就放弃
            if (umount2(target, MNT_DETACH) != 0) return -1;
        }
        n++;
    }
    return n;
}

// 写日志（带时间戳，超过 64KB 自动截断）
static void logf(const char *fmt, ...) {
    struct stat st;
    if (stat(LOG_FILE, &st) == 0 && st.st_size > 64 * 1024) {
        // 保留后一半
        int rf = open(LOG_FILE, O_RDONLY | O_CLOEXEC);
        if (rf >= 0) {
            char *tmp = (char *)malloc(st.st_size + 1);
            if (tmp) {
                ssize_t rn = read(rf, tmp, st.st_size);
                close(rf);
                if (rn > 0) {
                    int wf = open(LOG_FILE, O_WRONLY | O_TRUNC | O_CLOEXEC);
                    if (wf >= 0) {
                        write(wf, tmp + rn / 2, rn - rn / 2);
                        close(wf);
                    }
                }
                free(tmp);
            } else close(rf);
        }
    }

    // 日志目录可能还不存在（首次安装、scan 早于 apply 执行）——
    // 逐级创建，否则所有日志都会静默丢失，出问题无法排查。
    {
        std::string p;
        for (size_t i = 1; i < g_work.size(); i++) {
            if (g_work[i] == '/') {
                p = g_work.substr(0, i);
                mkdir(p.c_str(), 0755);
            }
        }
        mkdir(g_work.c_str(), 0755);
    }

    int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    char line[512];
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    int n = snprintf(line, sizeof(line), "%02d-%02d %02d:%02d:%02d ",
                     tmv.tm_mon + 1, tmv.tm_mday,
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    if (n < (int)sizeof(line) - 1) { line[n++] = '\n'; }
    ssize_t w = write(fd, line, n);
    (void)w;
    close(fd);
}

static bool is_mounted(const char *target) {
    FILE *fp = fopen("/proc/self/mountinfo", "r");
    if (!fp) return false;
    char line[2048];
    bool found = false;
    size_t tlen = strlen(target);
    while (fgets(line, sizeof(line), fp)) {
        // mountinfo 格式: id parent major:minor root MOUNTPOINT ...
        // 第 5 个字段是挂载点（1-based）
        char *p = line;
        int field = 0;
        while (*p && field < 4) {
            if (*p == ' ') field++;
            p++;
        }
        if (field < 4) continue;
        // 此时 p 指向第 5 字段开头
        if (strncmp(p, target, tlen) == 0 &&
            (p[tlen] == ' ' || p[tlen] == '\n' || p[tlen] == 0)) {
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

static bool excluded_type(const std::string &t) {
    for (int i = 0; EXCLUDE_TYPE[i]; i++)
        if (t == EXCLUDE_TYPE[i]) return true;
    for (int i = 0; EXCLUDE_SUBSTR[i]; i++)
        if (t.find(EXCLUDE_SUBSTR[i]) != std::string::npos) return true;
    return false;
}

// 判断该 zone 是否需要伪装（排除无效传感器）
static bool zone_wanted(const std::string &type) {
    if (excluded_type(type)) return false;
    // cpu-hw-trip 是硬件跳闸阈值，必须保留真实值
    if (type.find("cpu-hw-trip") != std::string::npos) return false;
    return true;
}

// 逐级创建目录（mkdir 单层在父目录不存在时会 ENOENT 失败，
// 而 fake/real 目录创建失败会导致后续 open 全部失败、节点静默跳过）
static void ensure_dir(const char *p) {
    if (!p || !*p) return;
    std::string s(p);
    for (size_t i = 1; i < s.size(); i++) {
        if (s[i] == '/') {
            std::string sub = s.substr(0, i);
            if (mkdir(sub.c_str(), 0755) != 0 && errno != EEXIST) { /* 忽略中间层错误 */ }
        }
    }
    if (mkdir(s.c_str(), 0755) != 0 && errno != EEXIST) {
        logf("[mkdir] 创建失败 %s: %s", p, strerror(errno));
    }
}

// ============ 按 type 名查找 zone（替代硬编码编号）============
// 硬编码 zone 编号在不同固件/内核版本下会错位，按 type 名查找才可靠。
//
// 性能注意：热路径（温度墙每秒轮询）必须零扫描。
// zone 编号在开机后固定不变，因此首次查到的结果直接缓存，
// 否则每秒遍历 97 个 zone 的 type 文件会把 CPU 占用推到 10% 以上。

struct ZoneCache { std::string type; long zone; };
static std::vector<ZoneCache> g_zone_cache;

// 精确匹配 type，返回 zone 编号；找不到返回 -1
static long zone_by_type(const char *type) {
    // 先查缓存
    for (auto &c : g_zone_cache)
        if (c.type == type) return c.zone;

    DIR *d = opendir(ZONE_ROOT);
    if (!d) return -1;
    struct dirent *e;
    long found = -1;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
        std::string tp = std::string(ZONE_ROOT) + "/" + e->d_name + "/type";
        std::string t;
        if (!read_str(tp.c_str(), t)) continue;
        if (t == type) { found = atol(e->d_name + 12); break; }
    }
    closedir(d);
    // 命中则缓存；未命中不缓存（可能该节点确实不存在，避免反复扫描也避免存 -1 后
    // 设备补上节点时永久失效——实际场景中 zone 不会热插拔，这里取保守策略）
    if (found >= 0) g_zone_cache.push_back({ type, found });
    return found;
}

// 按 type 前缀收集 zone 编号，返回数量
static int zones_by_prefix(const char *prefix, long *out, int maxn) {
    DIR *d = opendir(ZONE_ROOT);
    if (!d) return 0;
    struct dirent *e;
    int n = 0;
    size_t pl = strlen(prefix);
    while ((e = readdir(d)) && n < maxn) {
        if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
        std::string tp = std::string(ZONE_ROOT) + "/" + e->d_name + "/type";
        std::string t;
        if (!read_str(tp.c_str(), t)) continue;
        if (t.size() >= pl && t.compare(0, pl, prefix) == 0)
            out[n++] = atol(e->d_name + 12);
    }
    closedir(d);
    return n;
}


// ============ 安装时节点自动扫描与适配 ============

// 前向声明（定义见屏幕亮度区）
static const char *bl_dir();
static int  g_bl_max;
//
// 目的：本模块原先按 M098FE 的 zone 编号硬编码，换机型就可能错位或漏节点。
// 这里在安装/首次启动时做一次全量扫描，把结果落盘成 profile.conf，
// 之后 daemon 直接读这份 profile，不再依赖任何硬编码 zone 编号。
//
// 扫描产出四类信息：
//   1) 可伪装节点清单（zone 号 + type 名），排除危险节点
//   2) 关键传感器定位（电池真实温度源 / CPU / GPU / 充电口），按候选名优先级匹配
//   3) 充电控制点可用性（wired_chg_curr 等 xm_power 节点是否存在可写）
//   4) 屏幕背光面板定位（brightness 可写且有 max_brightness）
//
// 结果文件 profile.conf（纯文本，key=value，便于人工排查与 WebUI 读取）

struct ZoneRec {
    long zone;
    std::string type;
    int  temp;      // 当前读数(0.001℃)，扫描时快照
    bool is_dir;    // temp 节点存在
};

static std::vector<ZoneRec> g_all_zones;

// 全量扫描所有 thermal_zone，结果放 g_all_zones（按 zone 号升序）
static int scan_all_zones() {
    g_all_zones.clear();
    DIR *d = opendir(ZONE_ROOT);
    if (!d) {
        logf("[scan] 无法打开 %s: %s", ZONE_ROOT, strerror(errno));
        return -1;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
        ZoneRec r;
        r.zone = atol(e->d_name + 12);
        std::string base = std::string(ZONE_ROOT) + "/" + e->d_name;
        if (!read_str((base + "/type").c_str(), r.type)) continue;
        r.temp = 0;
        r.is_dir = false;
        {
            int v = 0;
            if (read_int((base + "/temp").c_str(), &v)) { r.temp = v; r.is_dir = true; }
        }
        r.is_dir = r.is_dir && (access((base + "/temp").c_str(), R_OK) == 0);
        g_all_zones.push_back(r);
    }
    closedir(d);
    std::sort(g_all_zones.begin(), g_all_zones.end(),
              [](const ZoneRec &a, const ZoneRec &b){ return a.zone < b.zone; });
    logf("[scan] 共发现 %d 个 thermal_zone", (int)g_all_zones.size());
    return (int)g_all_zones.size();
}

// 按候选名列表查找第一个存在的 zone（精确匹配），返回 -1 表示都没有
static long pick_zone(const char *const *cands) {
    for (int i = 0; cands[i]; i++)
        for (auto &z : g_all_zones)
            if (z.type == cands[i]) return z.zone;
    return -1;
}

// ============ 系统版本识别 ============
//
// 目的：OS3 与 OS4 的节点命名/语义大体一致但不保证完全相同，
// 用户也明确说"系统相同的话基本上都是差不多的"。
// 因此系统版本只作为**候选优先级提示与日志线索**，
// 真正的判据永远是下面的实测探测（改 fake_temp 看谁跟随）。
//
// 读取方式：不用 __system_property_get（该符号只由 Android bionic 导出，
// 本模块用 glibc 静态编译，链接期必然 undefined reference —— 已实测）。
// 改为直接执行 getprop 并捕获 stdout；getprop 在 /system/bin，root 下必在。
// 注意不能用 popen（glibc 硬编码 /bin/sh，Android 上是 /system/bin/sh）。
// 离线模拟测试可用 TCB_OS_CODE / TCB_OS_NAME 等环境变量直接注入，避免依赖设备。
static std::string run_capture(const char *exe, char *const argv[]); // 定义见后文

static std::string prop_str(const char *key) {
    // 1) 环境变量覆盖（离线测试 / 特殊机型）
    if (strcmp(key, "ro.mi.os.version.code") == 0) {
        const char *v = getenv("TCB_OS_CODE");
        if (v && *v) return std::string(v);
    }
    if (strcmp(key, "ro.mi.os.version.name") == 0) {
        const char *v = getenv("TCB_OS_NAME");
        if (v && *v) return std::string(v);
    }
    // 2) 走 getprop
    char *argv[] = { (char*)"getprop", (char*)key, nullptr };
    const char *exes[] = { "/system/bin/getprop", "getprop" };
    for (int i = 0; i < 2; i++) {
        std::string s = run_capture(exes[i], argv);
        // 去掉尾部空白（getprop 输出带换行）
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                              s.back() == ' '  || s.back() == '\t')) s.pop_back();
        if (!s.empty()) return s;
    }
    return std::string();
}

static int prop_int(const char *key, int def) {
    std::string s = prop_str(key);
    if (s.empty()) return def;
    return atoi(s.c_str());
}

// OS 主版本：3=OS3, 4=OS4, 0=未知（非澎湃或读不到）
static int os_major() {
    static int cached = -1;
    if (cached >= 0) return cached;
    cached = prop_int("ro.mi.os.version.code", 0);
    return cached;
}

static std::string os_desc() {
    char b[160];
    snprintf(b, sizeof(b), "OS%s(%s) Android%s SDK%d",
             prop_str("ro.mi.os.version.name").empty() ? "?" :
                 prop_str("ro.mi.os.version.name").c_str(),
             prop_str("ro.mi.os.version.code").empty() ? "?" :
                 prop_str("ro.mi.os.version.code").c_str(),
             prop_str("ro.build.version.release").empty() ? "?" :
                 prop_str("ro.build.version.release").c_str(),
             prop_int("ro.build.version.sdk", 0));
    return std::string(b);
}

// 该 type 是否"看起来像电池温度节点"（模糊匹配，覆盖未知命名）
static bool looks_battery(const std::string &t) {
    if (t.find("batt") != std::string::npos) return true;
    if (t.find("BATT") != std::string::npos) return true;
    if (t.find("fuelgauge") != std::string::npos) return true;
    return false;
}

// 该 type 是否明确是"被 fake_temp 改写"的已知污染节点
static bool known_polluted_name(const std::string &t) {
    return t == "battery" || t == "batt_temp" || t == "BATT_TEMP";
}

// ============ 电池温度节点语义分组 ============
//
// 为什么必须分组（实测 M098FE / HyperOS3，用 unshare 剥离挂载后做的扰动实验）：
//
//   fake_temp=350  →  zone67 battery = 35000   zone70 batt_temp = 35000   zone85 batt_therm = 42566
//   fake_temp=420  →  zone67 battery = 42000   zone70 batt_temp = 42000   zone85 batt_therm = 42553
//                     ↑ 完全跟随                ↑ 完全跟随                  ↑ 纹丝不动
//
// 结论：battery / batt_temp / power_supply 会被 fake_temp 直接改写，
//       只有 batt_therm 是独立 NTC。拿被改写的节点做温度墙判断，
//       等于永远读到设定值（35℃），温度墙永不触发。
//
// 但不同机型命名未必相同，所以下面不只靠名字：
//   1) 先按已知候选名收集
//   2) 再用"改写 fake_temp 观察谁跟随"的实测方式验证可信性
//   3) 名字命中但实测不可信的，一律降级为兜底，不参与温度墙判断
static const char *BATT_TRUE_TYPES[] = { "batt_therm", nullptr };
static const char *BATT_FB_TYPES[]   = { "battery", "batt_temp", nullptr };

// 语义探测：临时改 fake_temp，看某 zone 是否跟随。
// 返回 true = 该节点跟随 fake_temp（被污染，不可信）
// 返回 false = 不跟随（真实独立传感器）
//
// 安全约束：
//   · 只在 scan（安装期）执行一次，不在热路径
//   · 探测前保存原值，结束后一定恢复
//   · 若 selftest 开关关闭（TCB_NO_SEMTEST=1）则跳过探测，退回按名字判断
// 语义探测结果（三态）。
//
// 为什么必须是三态而不是 bool：
// 原实现返回 bool，把"读取失败/写入失败/值不可判"一律折叠成 false（= 不跟随 = 可信），
// 结果是一旦探测过程出任何异常，污染节点就会被当成真实传感器用于温度墙判断 ——
// 这比"没探测成功"更危险。现在只有确凿观测到"不跟随"才算可信，
// 其它一切异常一律 UNKNOWN（视为不可信），宁可不控也不拿假值控。
enum {
    PROBE_TRUSTED  = 0,   // 不跟随 fake_temp ⇒ 真实独立传感器
    PROBE_POLLUTED = 1,   // 跟随 fake_temp   ⇒ 被改写，不可信
    PROBE_UNKNOWN  = 2    // 判定失败（读写异常 / 节点值不可判）⇒ 可信性未知
};

// 恢复 fake_temp 到指定值，带读回校验与重试。
//
// 为什么单独抽出来：探测过程有多条异常返回路径，若只在正常路径上恢复，
// 一旦中途 return 就永久留下污染值。所有恢复动作统一走这里，
// 并且失败必须留下显著日志（污染会直接改掉电池温度上报，属功能级问题）。
static bool restore_fake_temp(int orig) {
    for (int i = 0; i < 3; i++) {
        int cur = -1;
        if (read_int(N_FAKE_TEMP, &cur) && cur == orig) return true;
        if (write_int(N_FAKE_TEMP, orig)) {
            if (read_int(N_FAKE_TEMP, &cur) && cur == orig) return true;
        }
        usleep(100 * 1000);
    }
    logf("[semtest] 严重：fake_temp 未能恢复为 %d，请手动检查 %s", orig, N_FAKE_TEMP);
    return false;
}

// 语义探测：临时改 fake_temp，看某 zone 是否跟随。
//
// 安全约束（用户明确要求"实失测不成功不能留下污染"）：
//   · 只在 scan（安装期）执行，不在热路径
//   · 写入探测值之前先快照原值；此后无论走哪条返回路径，都由统一出口恢复
//   · 恢复带读回校验，失败打显著告警，绝不静默
//   · TCB_NO_SEMTEST=1 时整体跳过，返回 UNKNOWN（不是 TRUSTED）
static int zone_follows_fake_temp(long zn) {
    if (zn < 0) return PROBE_UNKNOWN;
    if (getenv("TCB_NO_SEMTEST")) {
        logf("[semtest] TCB_NO_SEMTEST 已设置，跳过语义探测");
        return PROBE_UNKNOWN;
    }

    // 原值快照。读不到原值就绝不能写（写了无从恢复）
    int orig = 0;
    if (!read_int(N_FAKE_TEMP, &orig)) {
        logf("[semtest] fake_temp 不可读，跳过语义探测（不会写入，无污染风险）");
        return PROBE_UNKNOWN;
    }

    char zp[256];
    snprintf(zp, sizeof(zp), "%s/thermal_zone%ld/temp", ZONE_ROOT, zn);

    int t0 = 0;
    if (!read_int(zp, &t0)) {
        logf("[semtest] zone%ld temp 不可读，跳过语义探测（未写入 fake_temp）", zn);
        return PROBE_UNKNOWN;
    }

    // 探测值：原值 +7℃，越界则 -7℃
    int probe = orig + 70;
    if (probe > 900) probe = orig - 70;
    if (probe < 0)   probe = 420;

    if (!write_int(N_FAKE_TEMP, probe)) {
        // 写入失败：本次没改动成功，但仍走统一恢复，防止"部分写入"之类的边缘情况
        logf("[semtest] 无法写 fake_temp，跳过语义探测");
        restore_fake_temp(orig);
        return PROBE_UNKNOWN;
    }

    // 驱动回写需要时间，实测 300ms 足够，留 600ms 余量
    usleep(600 * 1000);

    int t1 = 0;
    bool t1_ok = read_int(zp, &t1);

    // 统一出口：先恢复，再判定。恢复失败有独立日志，不影响判定流程继续输出诊断
    bool restored = restore_fake_temp(orig);

    if (!t1_ok) {
        logf("[semtest] zone%ld 探测期 temp 读回失败 ⇒ 判定不可用（可信性未知）", zn);
        return PROBE_UNKNOWN;
    }

    // 跟随判定：节点变化方向必须与探测值变化方向一致，且幅度够大。
    //
    // 单位自适应（必须有）：
    //   常见 thermal_zone/temp 用毫摄氏度（35000 = 35℃），但存在用 0.1℃ 的机型（350）。
    //   阈值若写死毫摄氏度量级，在 0.1℃ 机型上"完全跟随的 Δ=70"会落进
    //   "纹丝不动"区间，把污染节点误判成可信传感器 —— 离线测试已实测复现。
    //   判据用 t0 的量级：|t0| ≥ 10000 当毫摄氏度，否则当 0.1℃（阈值缩 100 倍）。
    int scale = (t0 >= 10000 || t0 <= -10000) ? 1 : 100;

    // 阈值依据（实测 M098FE / OS3，600ms 窗口，毫摄氏度单位）：
    //   · 真实 NTC 自然漂移 ≤ 13mC（0.13℃），远小于 250mC
    //   · 跟随节点会整段跟随，delta ≈ ±7000（7℃）
    // 所以 |delta| < 250 ⇒ 判"没跟随"；250~500 模糊区 ⇒ 不敢下结论；
    // ≥500 且方向一致 ⇒ 确凿跟随。
    int th_flat   = 250 / scale;
    int th_follow = 500 / scale;
    if (th_flat   < 1) th_flat   = 1;
    if (th_follow < 2) th_follow = 2;

    int delta  = t1 - t0;
    int pdelta = probe - orig;
    int adelta = delta < 0 ? -delta : delta;
    int apdel  = pdelta < 0 ? -pdelta : pdelta;

    int res;
    if (apdel == 0) {
        res = PROBE_UNKNOWN;                        // 探测值等于原值，等于没探测
    } else if (adelta < th_flat) {
        res = PROBE_TRUSTED;                        // 纹丝不动 ⇒ 独立传感器
    } else if (adelta >= th_follow && ((delta > 0) == (pdelta > 0))) {
        res = PROBE_POLLUTED;                       // 整段跟随 ⇒ 被改写
    } else {
        res = PROBE_UNKNOWN;                        // 模糊区 ⇒ 不可判，按不可信处理
    }

    logf("[semtest] zone%ld 探测：fake_temp %d→%d，节点 %dmC→%dmC（Δ=%d，单位缩放 1/%d，阈值 %d/%d）⇒ %s%s",
         zn, orig, probe, t0, t1, delta, scale, th_flat, th_follow,
         res == PROBE_TRUSTED  ? "不跟随（真实传感器）" :
         res == PROBE_POLLUTED ? "跟随（被污染，不可信）" : "判定失败（可信性未知）",
         restored ? "" : "  [恢复异常]");
    return res;
}

// ============ 单调时钟（全局公共）============
//
// 全局统一用它做「时间差」计算，不用 time(nullptr)：
//   手机上的 wall clock 随时可能跳变 —— 开机早期 RTC 未同步、NTP 回拨、
//   用户手动改时间、时区切换。任何「多久之后该做某事」「多久之前写的」判断
//   一旦用了 wall clock，跳变就会让它永久失效或瞬间误触发。
//   已实测踩过三处：快照刷新停摆、快照新鲜度判定失效、温度墙积分项失灵。
static long mono_ms() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// 节点是否可写（判断充电控制点用）
static bool node_writable(const std::string &p) {
    return access(p.c_str(), W_OK) == 0;
}

static bool node_readable(const std::string &p) {
    return access(p.c_str(), R_OK) == 0;
}

struct Profile {
    int  ver;
    int  os_major;              // 澎湃 OS 主版本（3/4/0=未知），扫描时记录
    std::string os_desc;        // 完整系统描述，便于排查
    int  zone_total;
    int  mask_count;            // 可伪装节点数
    int  skip_count;            // 被排除节点数

    // 电池温度源。语义必须分级，不能笼统叫"真实温度源"：
    //   batt_zone / batt_type —— 仅当找到独立 NTC（batt_therm 类）时才填。
    //   batt_trusted          —— 1=真实可信，0=不可信（只有被 fake_temp 污染的节点）
    //   batt_fb_zone/-type    —— 兜底节点（battery/batt_temp），仅供显示参考
    // 用途：温度墙只有在 batt_trusted=1 时才允许启用，否则等于拿假值做判断。
    long batt_zone;
    std::string batt_type;
    int  batt_trusted;
    long batt_fb_zone;
    std::string batt_fb_type;

    long cpu_first;             // 第一个 CPU zone（诊断用）
    long gpu_first;
    long conn_zone;             // 充电口
    std::string conn_type;
    bool ctrl_ok, pps_ok, cur_ok, fake_ok;
    bool thermal_ok;            // OS4: wired_thermal_remove 是否可写（OS3 无此节点=0）
    std::string bl_dir;
    int  bl_max;
    std::string stamp;
};

static Profile g_prof;

static std::string pf_path() { return std::string(WORK_DIR) + "/profile.conf"; }

static bool save_profile() {
    ensure_dir(WORK_DIR);
    FILE *fp = fopen(pf_path().c_str(), "w");
    if (!fp) {
        logf("[scan] profile 写入失败: %s", strerror(errno));
        return false;
    }
    fprintf(fp, "ver=%d\n",            g_prof.ver);
    fprintf(fp, "os_major=%d\n",       g_prof.os_major);
    fprintf(fp, "os_desc=%s\n",        g_prof.os_desc.c_str());
    fprintf(fp, "zone_total=%d\n",     g_prof.zone_total);
    fprintf(fp, "mask_count=%d\n",     g_prof.mask_count);
    fprintf(fp, "skip_count=%d\n",     g_prof.skip_count);
    fprintf(fp, "batt_zone=%ld\n",     g_prof.batt_zone);
    fprintf(fp, "batt_type=%s\n",      g_prof.batt_type.c_str());
    fprintf(fp, "batt_trusted=%d\n",   g_prof.batt_trusted);
    fprintf(fp, "batt_fb_zone=%ld\n",  g_prof.batt_fb_zone);
    fprintf(fp, "batt_fb_type=%s\n",   g_prof.batt_fb_type.c_str());
    fprintf(fp, "cpu_first=%ld\n",     g_prof.cpu_first);
    fprintf(fp, "gpu_first=%ld\n",     g_prof.gpu_first);
    fprintf(fp, "conn_zone=%ld\n",     g_prof.conn_zone);
    fprintf(fp, "conn_type=%s\n",      g_prof.conn_type.c_str());
    fprintf(fp, "ctrl_ok=%d\n",        g_prof.ctrl_ok ? 1 : 0);
    fprintf(fp, "thermal_ok=%d\n",     g_prof.thermal_ok ? 1 : 0);
    fprintf(fp, "pps_ok=%d\n",         g_prof.pps_ok ? 1 : 0);
    fprintf(fp, "cur_ok=%d\n",         g_prof.cur_ok ? 1 : 0);
    fprintf(fp, "fake_ok=%d\n",        g_prof.fake_ok ? 1 : 0);
    fprintf(fp, "bl_dir=%s\n",         g_prof.bl_dir.c_str());
    fprintf(fp, "bl_max=%d\n",         g_prof.bl_max);
    fprintf(fp, "stamp=%s\n",          g_prof.stamp.c_str());
    fclose(fp);
    return true;
}

static bool load_profile() {
    FILE *fp = fopen(pf_path().c_str(), "r");
    if (!fp) return false;
    char line[512];
    g_prof = Profile{};
    g_prof.batt_zone = g_prof.cpu_first = g_prof.gpu_first = g_prof.conn_zone = -1;
    g_prof.batt_fb_zone = -1;
    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        std::string k = line, v = eq + 1;
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
        if      (k == "ver")         g_prof.ver = atoi(v.c_str());
        else if (k == "os_major")    g_prof.os_major = atoi(v.c_str());
        else if (k == "os_desc")     g_prof.os_desc = v;
        else if (k == "zone_total")  g_prof.zone_total = atoi(v.c_str());
        else if (k == "mask_count")  g_prof.mask_count = atoi(v.c_str());
        else if (k == "skip_count")  g_prof.skip_count = atoi(v.c_str());
        else if (k == "batt_zone")   g_prof.batt_zone = atol(v.c_str());
        else if (k == "batt_type")   g_prof.batt_type = v;
        else if (k == "batt_trusted")g_prof.batt_trusted = atoi(v.c_str());
        else if (k == "batt_fb_zone")g_prof.batt_fb_zone = atol(v.c_str());
        else if (k == "batt_fb_type")g_prof.batt_fb_type = v;
        else if (k == "cpu_first")   g_prof.cpu_first = atol(v.c_str());
        else if (k == "gpu_first")   g_prof.gpu_first = atol(v.c_str());
        else if (k == "conn_zone")   g_prof.conn_zone = atol(v.c_str());
        else if (k == "conn_type")   g_prof.conn_type = v;
        else if (k == "ctrl_ok")     g_prof.ctrl_ok = atoi(v.c_str()) != 0;
        else if (k == "thermal_ok")  g_prof.thermal_ok = atoi(v.c_str()) != 0;
        else if (k == "pps_ok")      g_prof.pps_ok  = atoi(v.c_str()) != 0;
        else if (k == "cur_ok")      g_prof.cur_ok  = atoi(v.c_str()) != 0;
        else if (k == "fake_ok")     g_prof.fake_ok = atoi(v.c_str()) != 0;
        else if (k == "bl_dir")      g_prof.bl_dir = v;
        else if (k == "bl_max")      g_prof.bl_max = atoi(v.c_str());
        else if (k == "stamp")       g_prof.stamp = v;
    }
    fclose(fp);
    return g_prof.ver > 0;
}

// 扫描并生成 profile。install=true 时额外打印详细清单（安装期看得到）
// 前向声明：持久 fd 相关（定义见「温度伪装」区之后的持久 fd 区）
static void open_real_fds();
static void close_real_fds();
static void flush_real_values();
static int  read_live_real(long zn);
// 前向声明：wall_real_batt_temp()（约 1535 行）在 snapshot_fresh() 定义（约 1960 行）
// 之前就用到它。温度墙必须与 dump 用同一套新鲜度判据，否则两条路径判定不一致。
static bool snapshot_fresh();

static int do_scan(bool verbose) {
    logf("[scan] === 开始扫描温度节点 ===");
    int total = scan_all_zones();
    if (total <= 0) {
        logf("[scan] 失败：没有发现任何 thermal_zone，伪装功能将不可用");
        return -1;
    }

    int mask = 0, skip = 0;
    for (auto &z : g_all_zones) {
        if (zone_wanted(z.type)) {
            mask++;
            if (verbose) logf("[scan]   伪装 zone%-3ld %-22s %d.%d℃",
                              z.zone, z.type.c_str(), z.temp / 1000, (z.temp % 1000) / 100);
        } else {
            skip++;
            if (verbose) logf("[scan]   跳过 zone%-3ld %-22s （危险/无效节点）",
                              z.zone, z.type.c_str());
        }
    }

    // ============ 电池温度源选取（语义实测判定，不靠名字猜）============
    //
    // ============ 电池温度源：按系统版本给候选，用实测定语义 ============
    //
    // 用户要求：不同机型节点不同，但同系统基本一致 → 所以：
    //   1) 系统版本只用来**排列候选优先级**（OS3/OS4 已知命名不同则各用各的顺序）
    //   2) 真正决定"哪个能用作真实源"的，是对每个候选做 fake_temp 扰动实测
    //
    // 这样 OS3 / OS4 / 未知系统 / 未来新命名，都走同一条判定路径，
    // 不会因为我把某个名字写死而"取错语义"。
    {
        logf("[scan] 系统：%s", os_desc().c_str());
        int osm = os_major();
        if (osm == 3)      logf("[scan] 识别为 OS3，使用 OS3 候选顺序");
        else if (osm == 4) logf("[scan] 识别为 OS4，使用 OS4 候选顺序");
        else               logf("[scan] 未能识别澎湃版本，使用通用候选顺序");
    }

    // 候选顺序：把"更像独立 NTC"的名字放前面，仅影响尝试顺序，不影响判定结果
    static const char *BATT_ORDER_OS3[] = { "batt_therm", "battery", "batt_temp", nullptr };
    static const char *BATT_ORDER_OS4[] = { "batt_therm", "batt_temp", "battery", nullptr };
    static const char *BATT_ORDER_GEN[] = { "batt_therm", "batt_temp", "battery",
                                            "batt_therm_temp", "battery_temp", nullptr };
    const char *const *order =
        os_major() == 3 ? BATT_ORDER_OS3 :
        os_major() == 4 ? BATT_ORDER_OS4 : BATT_ORDER_GEN;

    // 收集候选：先按已知顺序列表，再补上任何"看起来像电池"的未知节点
    std::vector<long> cand;
    std::vector<std::string> cand_t;
    for (int i = 0; order[i]; i++) {
        // 单个名字查找：pick_zone 要的是 nullptr 结尾的数组，这里直接按名字比
        long z = -1;
        for (auto &zz : g_all_zones)
            if (zz.type == order[i]) { z = zz.zone; break; }
        if (z < 0) continue;
        bool dup = false;
        for (size_t k = 0; k < cand.size(); k++) if (cand[k] == z) dup = true;
        if (dup) continue;
        std::string t;
        for (auto &zz : g_all_zones) if (zz.zone == z) t = zz.type;
        cand.push_back(z); cand_t.push_back(t);
    }
    // 补充未知命名的电池节点（换系统/换机型时的新名字也能纳入实测）
    for (auto &zz : g_all_zones) {
        if (!looks_battery(zz.type)) continue;
        bool dup = false;
        for (size_t k = 0; k < cand.size(); k++) if (cand[k] == zz.zone) dup = true;
        if (dup) continue;
        cand.push_back(zz.zone); cand_t.push_back(zz.type);
        logf("[scan] 发现未知命名的电池类节点 zone%ld (%s)，纳入实测",
             zz.zone, zz.type.c_str());
    }

    logf("[scan] 电池类候选节点 %d 个，开始逐个实测语义", (int)cand.size());

    // 对每个候选做实测：跟随 fake_temp → 污染；不跟随 → 真实可信。
    //
    // 探测次数收敛（用户要求：不要实测太多，避免安装时太麻烦）：
    //   · 最多实测 2 个候选（每次要改 fake_temp + 等 600ms，多了会拖慢安装）
    //   · 一旦确认某个是独立传感器就立即停止
    //   · 实测过的即使被污染也会被记进兜底位，够判断了
    // 依据：同一系统版本下电池节点语义稳定，正常机型第 1 个（batt_therm）就命中，
    // 极少数命名不同才需要第 2 个。再多的候选按"已知污染名"直接归类，不做实测。
    const int MAX_PROBE = 2;
    long bz = -1, fbz = -1;
    std::string bt, fbt;
    int probed = 0;
    int n_unknown = 0;    // 语义判定失败（可信性未知）计数
    int n_mount_bad = 0;  // 探测后挂载恢复失败计数

    // 整个扫描开始前快照 fake_temp，末尾再做一次全局兜底恢复。
    // 为什么需要第二层：单个探测内部已有统一出口恢复，但若进程被中途打断
    // （安装脚本被杀 / OOM / 用户中断），仍可能留下探测值。
    // 这里保证「扫描结束态 == 扫描开始态」。
    int fake_snapshot = -1;
    bool fake_snapshot_ok = read_int(N_FAKE_TEMP, &fake_snapshot);

    for (size_t i = 0; i < cand.size(); i++) {
        long z = cand[i];
        const std::string &t = cand_t[i];

        // 已达探测上限：不再实测，仅按已知语义归类
        if (probed >= MAX_PROBE || bz >= 0) {
            if (known_polluted_name(t)) {
                if (fbz < 0) { fbz = z; fbt = t; }
                logf("[scan]   zone%ld %-14s ⇒ 已知污染名，归为兜底（未实测）",
                     z, t.c_str());
            } else {
                logf("[scan]   zone%ld %-14s ⇒ 未实测（探测次数已够），忽略",
                     z, t.c_str());
            }
            continue;
        }

        char tgt[256];
        snprintf(tgt, sizeof(tgt), "%s/thermal_zone%ld/temp", ZONE_ROOT, z);

        // 探测前剥掉自己的挂载，否则读到 350 伪装值，看不出跟不跟随
        bool was_mounted = is_mounted(tgt);
        if (was_mounted) {
            for (int k = 0; k < 5; k++) { if (umount2(tgt, MNT_DETACH) != 0) break; }
        }

        int res = zone_follows_fake_temp(z);
        probed++;

        // 恢复挂载，并校验真的挂回去了（失败要留痕，不能静默丢伪装）
        if (was_mounted) {
            char fz[320];
            snprintf(fz, sizeof(fz), "%s/thermal_zone%ld", FAKE_DIR, z);
            if (access(fz, F_OK) == 0) {
                if (mount(fz, tgt, nullptr, MS_BIND, nullptr) != 0 || !is_mounted(tgt)) {
                    n_mount_bad++;
                    logf("[scan]   警告：zone%ld 挂载恢复失败（%s）", z, strerror(errno));
                }
            }
        }

        if (res == PROBE_POLLUTED) {
            if (fbz < 0) { fbz = z; fbt = t; }
            logf("[scan]   zone%ld %-14s ⇒ 受 fake_temp 改写，不可信%s",
                 z, t.c_str(), known_polluted_name(t) ? "（与已知语义一致）" : "");
        } else if (res == PROBE_TRUSTED) {
            bz = z; bt = t;
            logf("[scan]   zone%ld %-14s ⇒ 独立传感器，判为真实温度源（停止探测）",
                 z, t.c_str());
        } else {
            // 判定失败：绝不能当成可信源（原实现会把这种情况判成"可信"，是主要隐患）
            n_unknown++;
            if (known_polluted_name(t) && fbz < 0) { fbz = z; fbt = t; }
            logf("[scan]   zone%ld %-14s ⇒ 语义判定失败，不采用（按不可信处理）",
                 z, t.c_str());
        }
    }

    // 扫描末尾兜底：把 fake_temp 恢复到扫描前的值
    if (fake_snapshot_ok) {
        if (!restore_fake_temp(fake_snapshot))
            logf("[scan] 警告：fake_temp 未能恢复到扫描前取值 %d", fake_snapshot);
    }

    logf("[scan] 语义探测完成：实测 %d 个（判定失败 %d），真实源=%s，兜底=%s",
         probed, n_unknown,
         bz >= 0 ? bt.c_str() : "无",
         fbz >= 0 ? fbt.c_str() : "无");
    if (n_mount_bad)
        logf("[scan] 警告：%d 个节点挂载恢复失败，伪装可能缺项", n_mount_bad);
    if (bz < 0)
        logf("[scan] 未找到可信电池温度源 ⇒ 温度墙将停用（宁可不控，也不拿假值控温）");

    static const char *CPU_CANDS[] = { "cpu-0-0-0", "cpu-0-0", nullptr };
    static const char *GPU_CANDS[] = { "gpuss-0", "gpu-0-0", "gpu", nullptr };
    static const char *CONN_CANDS[] = { "conn_therm", "conn_therm2", "charger_therm", nullptr };
    long cz = pick_zone(CONN_CANDS);
    std::string ct;
    for (auto &z : g_all_zones) if (z.zone == cz) ct = z.type;

    g_prof = Profile{};
    g_prof.ver        = 1;
    g_prof.os_major   = os_major();
    g_prof.os_desc    = os_desc();
    g_prof.zone_total = total;
    g_prof.mask_count = mask;
    g_prof.skip_count = skip;
    g_prof.batt_zone  = bz;
    g_prof.batt_type  = bt;
    // trusted 只在"名字命中 + 实测不跟随 fake_temp"时才是 1
    g_prof.batt_trusted = (bz >= 0) ? 1 : 0;
    g_prof.batt_fb_zone = fbz;
    g_prof.batt_fb_type = fbt;
    g_prof.cpu_first  = pick_zone(CPU_CANDS);
    g_prof.gpu_first  = pick_zone(GPU_CANDS);
    g_prof.conn_zone  = cz;
    g_prof.conn_type  = ct;
    g_prof.ctrl_ok    = node_writable(N_CTRL_LIMIT);
    g_prof.pps_ok     = node_writable(N_PPS_PTF);
    g_prof.cur_ok     = node_writable(N_WIRED_CURR);
    g_prof.fake_ok    = node_writable(N_FAKE_TEMP);
    // OS4 新增：wired_thermal_remove 是「充电温度限制解除」开关。
    //   写 1 = 解除 IC 级温度限流，实测电流 5.33A → 6.07A（+0.74A）；
    //   写 0 = 恢复厂商默认限流。
    // OS3 无此节点 → node_writable 返回 false → thermal_ok=0 → 全流程自动跳过。
    g_prof.thermal_ok = node_writable(N_THERMAL_RM);
    {
        char ts[32];
        time_t now = time(nullptr);
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
        g_prof.stamp = ts;
    }

    // 背光面板
    {
        const char *dir = bl_dir();
        if (dir) { g_prof.bl_dir = dir; g_prof.bl_max = g_bl_max; }
    }

    if (!save_profile()) return -1;

    logf("[scan] 完成：共 %d 个 zone，可伪装 %d，跳过 %d", total, mask, skip);
    if (bz >= 0) {
        logf("[scan] 电池真实温度源：zone%ld (%s) ✓ 独立 NTC，不受 fake_temp 影响",
             bz, bt.c_str());
    } else {
        logf("[scan] 电池真实温度源：未找到独立 NTC（batt_therm）");
        if (fbz >= 0)
            logf("[scan]   仅有兜底节点 zone%ld (%s)，该节点被 fake_temp 改写，"
                 "伪装后恒为设定值，不可用于温度墙判断", fbz, fbt.c_str());
    }
    logf("[scan] 充电控制点：curr=%s ctrl=%s pps=%s fake=%s",
         g_prof.cur_ok ? "可写" : "缺失",
         g_prof.ctrl_ok ? "可写" : "缺失",
         g_prof.pps_ok ? "可写" : "缺失",
         g_prof.fake_ok ? "可写" : "缺失");
    logf("[scan] 屏幕背光：%s（max=%d）",
         g_prof.bl_dir.empty() ? "未找到，亮度功能禁用" : g_prof.bl_dir.c_str(),
         g_prof.bl_max);

    if (bz < 0)
        logf("[scan] 警告：找不到电池温度节点，充电温度墙将不可用");
    if (!g_prof.cur_ok)
        logf("[scan] 警告：%s 不可写，充电温度墙/提速将不可用", N_WIRED_CURR);
    return total;
}

static int apply_temp(int want) {
    ensure_dir(WORK_DIR);
    ensure_dir(FAKE_DIR);
    ensure_dir(REAL_DIR);

    // 没有任何 profile 时先扫一遍：这就是「安装后首次启动自动适配」的落点。
    // 已有 profile 则直接沿用，避免每次开机都全量扫描。
    if (!load_profile()) {
        logf("[apply] 未找到 profile.conf，执行首次节点扫描");
        if (do_scan(false) <= 0) {
            logf("[apply] 扫描失败，无法建立伪装（温度节点不可访问）");
            fprintf(stderr, "[temp] scan failed\n");
            return -1;
        }
    } else {
        logf("[apply] 使用已有 profile（生成于 %s，zone %d 个，可伪装 %d）",
             g_prof.stamp.c_str(), g_prof.zone_total, g_prof.mask_count);
    }

    // 关键：必须在挂载前打开持久 fd，才能绕过 bind mount 读到真实值
    open_real_fds();

    // 确保 fake_temp 也设上（官方接口，联动 power_supply）
    if (!write_int(N_FAKE_TEMP, want))
        logf("[apply] 警告：fake_temp 写入失败（%s），电池温度伪装可能不生效", N_FAKE_TEMP);

    int ok = 0, skip = 0, fail = 0;
    DIR *d = opendir(ZONE_ROOT);
    if (!d) return -1;

    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;

        std::string base = std::string(ZONE_ROOT) + "/" + e->d_name;
        std::string type_p = base + "/type";

        std::string type;
        if (!read_str(type_p.c_str(), type)) continue;
        if (!zone_wanted(type)) { skip++; continue; }

        // === 关键：挂载前先记录真实温度到 REAL_DIR ===
        std::string realf = std::string(REAL_DIR) + "/" + e->d_name;
        {
            int rv = 0;
            std::string tp = base + "/temp";
            if (read_int(tp.c_str(), &rv) && rv > 0) {
                int fd2 = open(realf.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
                if (fd2 >= 0) {
                    char b[16];
                    int n2 = snprintf(b, sizeof(b), "%d\n", rv);
                    ssize_t w2 = write(fd2, b, n2); (void)w2;
                    close(fd2);
                }
            }
        }

        // 假值文件
        std::string fake = std::string(FAKE_DIR) + "/" + e->d_name;
        int fd = open(fake.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            logf("[apply] 跳过 zone%s：创建假值文件失败 %s (%s)",
                 e->d_name + 12, fake.c_str(), strerror(errno));
            fail++;
            continue;
        }
        char buf[16];
        int n = snprintf(buf, sizeof(buf), "%d\n", want);
        ssize_t w = write(fd, buf, n); (void)w;
        close(fd);

        std::string tgt = base + "/temp";
        // 必须先把该目标剥到 0 层再挂；否则 sysfs 上重复 bind 会翻倍膨胀
        if (unmount_all(tgt.c_str()) < 0) {
            logf("[apply] 跳过 zone%s：清理旧挂载失败", e->d_name + 12);
            skip++;
            continue;
        }
        if (mount(fake.c_str(), tgt.c_str(), nullptr, MS_BIND, nullptr) == 0) {
            ok++;
        } else {
            // 不能静默失败：挂不上就等于该节点没伪装，必须能在日志里看到原因
            logf("[apply] 挂载失败 zone%s (%s): %s",
                 e->d_name + 12, type.c_str(), strerror(errno));
            fail++;
        }
    }
    closedir(d);

    // 充电提速节点
    write_int(N_CTRL_LIMIT, 0);
    write_int(N_PPS_PTF, 1);

    // OS4：解除 IC 级充电温度限流（OS3 无此节点，thermal_ok=0 自动跳过）。
    // 这是实测最有效的提速手段：+0.74A，且不需要改 wired_chg_curr 的档位。
    if (g_prof.thermal_ok) {
        if (write_int(N_THERMAL_RM, 1))
            logf("[apply] 已解除充电温度限流 (wired_thermal_remove=1)");
        else
            logf("[apply] 警告：解除充电温度限流失败 (%s)", N_THERMAL_RM);
    }

    // 挂载完成后写入真实值快照（供 WebUI 跨进程读取）
    flush_real_values();

    fprintf(stderr, "[temp] mounted=%d skipped=%d failed=%d\n", ok, skip, fail);
    logf("应用温度伪装: 挂载 %d 个, 跳过 %d 个, 失败 %d 个, 目标 %d",
         ok, skip, fail, want);
    if (fail > 0)
        logf("[apply] 警告：%d 个节点伪装失败，这些节点的温度对系统仍可见", fail);
    if (ok == 0)
        logf("[apply] 严重：没有任何节点伪装成功，功能等于未生效（检查 SELinux / 挂载权限）");
    return ok;
}

static int restore_temp() {
    int n = 0;
    // 先关掉持久 fd（避免 fd 泄漏）
    close_real_fds();

    DIR *d = opendir(ZONE_ROOT);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
            std::string tgt = std::string(ZONE_ROOT) + "/" + e->d_name + "/temp";
            // 剥到 0 层（含懒卸载兜底）
            int r = unmount_all(tgt.c_str());
            if (r > 0) n += r;
        }
        closedir(d);
    }
    // 恢复官方伪装接口
    write_int(N_FAKE_TEMP, -999);

    // 恢复充电相关节点。
    // 为什么必须有：温度墙可能正把充电电流限在最低 3A。只解挂载不恢复电流，
    // 卸载/restore 后充电会永久卡在限流值（用户看着像"充电变慢了"，很难自查）。
    // 放在最后：前面若失败也一定要走到这里。
    int cur = 0;
    if (read_int(N_WIRED_CURR, &cur) && cur > 0 && cur < DEF_WIRED_CURR) {
        write_int(N_WIRED_CURR, DEF_WIRED_CURR);
        logf("恢复充电电流: %d -> %d", cur, DEF_WIRED_CURR);
    }
    write_int(N_CTRL_LIMIT, 0);   // 放开控制器接管
    write_int(N_PPS_PTF, 1);

    fprintf(stderr, "[temp] restored=%d\n", n);
    logf("恢复温度伪装: 卸载 %d 个节点", n);
    return n;
}

// ============ 屏幕亮度 ============
// 实测事实（M098FE / HyperOS3 / 2026-10-28）：
//   /sys/class/backlight/panel0-backlight/brightness  可写，写入立即生效
//   max_brightness = 16383（只读）→ 合法范围 0~16383，不是 0~255
//   settings put system screen_brightness 9000 → sysfs 仍停在 1872（system_server 不回写）
//   反之写 sysfs 也不改 settings
//   ⇒ 必须双写：sysfs 保证即时生效，settings 保证重启后被系统读回
//
// 跨机：不硬编码 panel 名，扫 /sys/class/backlight/*/ 取第一个 brightness 可写
//       且 max_brightness > 0 的面板；全都不满足则整个功能自禁用（不影响其它功能）

static char g_bl_dir[320] = {0};
// 注：g_bl_max 已在上方「安装时节点自动扫描」区前向声明
// 背光根目录同样支持 TCB_ROOT 覆盖（离线模拟测试 / 非标准机型）
static std::string g_bl_root;

static const char *bl_dir() {
    if (g_bl_dir[0] && g_bl_max > 0) return g_bl_dir;
    if (g_bl_root.empty()) g_bl_root = env_or("TCB_BL_ROOT", std::string(g_root) + "/sys/class/backlight");
    DIR *d = opendir(g_bl_root.c_str());
    if (!d) return NULL;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char dir[320], br[352], mx[352];
        snprintf(dir, sizeof(dir), "%s/%s", g_bl_root.c_str(), e->d_name);
        struct stat st;
        if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        snprintf(br, sizeof(br), "%s/brightness", dir);
        snprintf(mx, sizeof(mx), "%s/max_brightness", dir);
        if (access(br, W_OK) != 0) continue;        // 必须可写
        int m = 0;
        if (!read_int(mx, &m) || m <= 0) continue;  // 必须有正的上限
        snprintf(g_bl_dir, sizeof(g_bl_dir), "%s", dir);
        g_bl_max = m;
        break;
    }
    closedir(d);
    return g_bl_dir[0] ? g_bl_dir : NULL;
}

// 执行外部命令（不经过 shell —— Android 上 popen 依赖 /bin/sh，不可靠）
static bool run_cmd(char *const argv[]) {
    pid_t p = fork();
    if (p < 0) return false;
    if (p == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); close(dn); }
        execv(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    if (waitpid(p, &st, 0) != p) return false;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

// 执行外部命令并捕获 stdout（仅用于 settings get）
static std::string run_capture(const char *exe, char *const argv[]) {
    int fd[2];
    if (pipe(fd) != 0) return std::string();
    pid_t p = fork();
    if (p < 0) { close(fd[0]); close(fd[1]); return std::string(); }
    if (p == 0) {
        close(fd[0]);
        dup2(fd[1], 1);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, 2); close(dn); }
        close(fd[1]);
        execv(exe, argv);
        _exit(127);
    }
    close(fd[1]);
    std::string out;
    char buf[128];
    ssize_t n;
    while ((n = read(fd[0], buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    close(fd[0]);
    int st = 0;
    waitpid(p, &st, 0);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

static const char *settings_bin() {
    static const char *cand[] = { "/system/bin/settings", "/system/xbin/settings", nullptr };
    for (int i = 0; cand[i]; i++)
        if (access(cand[i], X_OK) == 0) return cand[i];
    return NULL;
}

static void settings_put(const char *key, int val) {
    const char *exe = settings_bin();
    if (!exe) return;
    char vs[16];
    snprintf(vs, sizeof(vs), "%d", val);
    char *argv[6];
    argv[0] = (char *)exe;
    argv[1] = (char *)"put";
    argv[2] = (char *)"system";
    argv[3] = (char *)key;
    argv[4] = vs;
    argv[5] = nullptr;
    run_cmd(argv);
}

static int bl_auto_mode() {
    const char *exe = settings_bin();
    if (!exe) return -1;
    char *argv[5];
    argv[0] = (char *)exe;
    argv[1] = (char *)"get";
    argv[2] = (char *)"system";
    argv[3] = (char *)"screen_brightness_mode";
    argv[4] = nullptr;
    std::string s = run_capture(exe, argv);
    if (s.empty()) return -1;
    return atoi(s.c_str());
}

// 带缓存：dump 每 3 秒被 WebUI 调一次，不能每次都 fork 一次 settings
// 注意：dump 是独立进程，进程内缓存对它无效 → 状态落 bl_state.conf，
//       由 daemon（常驻）与 bright 命令共同维护，dump 只读文件，零 fork
static int  g_auto_cache = -1;
static long g_auto_ts    = 0;
static int bl_auto_mode_cached() {
    // 单调时钟：避免改时间后缓存永久命中/永久失效
    long now = mono_ms();
    if (g_auto_cache >= 0 && now - g_auto_ts < 30000) return g_auto_cache;
    int v = bl_auto_mode();
    if (v >= 0) { g_auto_cache = v; g_auto_ts = now; }
    return v;
}

static void bl_state_path(char *buf, size_t n) {
    snprintf(buf, n, "%s/bl_state.conf", WORK_DIR);
}

// 状态文件：auto pct raw（单行三字段，缺失表示未知）
static void bl_save_state(int auto_mode, int pct, int raw) {
    char p[192];
    bl_state_path(p, sizeof(p));
    ensure_dir(WORK_DIR);
    FILE *fp = fopen(p, "w");
    if (fp) { fprintf(fp, "%d %d %d\n", auto_mode, pct, raw); fclose(fp); }
}

// 读取状态文件；返回 false 表示文件缺失或解析失败
static bool bl_load_state(int *auto_mode, int *pct, int *raw) {
    char p[192];
    bl_state_path(p, sizeof(p));
    FILE *fp = fopen(p, "r");
    if (!fp) return false;
    int a = -1, c = 0, r = 0;
    int got = fscanf(fp, "%d %d %d", &a, &c, &r);
    fclose(fp);
    if (got < 3) return false;
    if (auto_mode) *auto_mode = a;
    if (pct)       *pct = c;
    if (raw)       *raw = r;
    return true;
}

static int bl_cur_raw() {
    const char *dir = bl_dir();
    if (!dir) return -1;
    char p[288];
    int v = -1;
    snprintf(p, sizeof(p), "%s/brightness", dir);
    if (!read_int(p, &v)) return -1;
    return v;
}

static int bl_pct_of(int raw) {
    if (g_bl_max <= 0 || raw < 0) return 0;
    // 四舍五入，避免 7372/16383 显示成 44% 而用户设的是 45%
    long long p = ((long long)raw * 100 + g_bl_max / 2) / g_bl_max;
    if (p < 1) p = 1;
    if (p > 100) p = 100;
    return (int)p;
}

static int bl_want_pct() {
    char cf[192];
    snprintf(cf, sizeof(cf), "%s/bright.conf", WORK_DIR);
    int p = 0;
    if (!read_int(cf, &p)) return 0;
    if (p < 1 || p > 100) return 0;
    return p;
}

// pct: 1~100
static bool bl_set_pct(int pct) {
    const char *dir = bl_dir();
    if (!dir) return false;
    if (pct < 1) pct = 1;
    if (pct > 100) pct = 100;
    int raw = (int)((long long)g_bl_max * pct / 100);
    if (raw < 1) raw = 1;
    if (raw > g_bl_max) raw = g_bl_max;

    // 只写 sysfs。实测结论（M098FE / 2026-10-28）：
    //   · 写 sysfs 后 12~15 秒观测稳定，不被回写
    //   · 写 settings screen_brightness 会触发 DisplayManager 按内部
    //     亮度曲线重算并反写 sysfs，反而把刚设的值改掉
    //   · settings 的 0~255 刻度经曲线映射后最大只能到 sysfs≈13106（80%），
    //     拿不到 16383 满亮度 → 所以 settings 不能作为亮度入口
    char p[320];
    snprintf(p, sizeof(p), "%s/brightness", dir);
    bool ok = write_int(p, raw);

    // 关自动亮度（这个只能走 settings，没有对应 sysfs；写它不会重置亮度）
    if (bl_auto_mode_cached() == 1) settings_put("screen_brightness_mode", 0);
    g_auto_cache = 0;
    g_auto_ts    = mono_ms();   // 必须与 bl_auto_mode_cached() 同一时钟（mono），否则缓存判据永远错
    bl_save_state(0, pct, raw);

    // 记下用户意图，守护进程据此纠正被系统改回的情况
    char cf[192];
    snprintf(cf, sizeof(cf), "%s/bright.conf", WORK_DIR);
    ensure_dir(WORK_DIR);
    FILE *fp = fopen(cf, "w");
    if (fp) { fprintf(fp, "%d\n", pct); fclose(fp); }
    return ok;
}

static bool bl_set_auto(int on) {
    if (!settings_bin()) return false;
    settings_put("screen_brightness_mode", on ? 1 : 0);
    g_auto_cache = on ? 1 : 0;
    g_auto_ts    = mono_ms();   // 同上：与缓存读取侧保持单调时钟一致
    bl_save_state(on ? 1 : 0, bl_want_pct(), bl_cur_raw());
    return true;
}

// 守护进程周期纠正：只在「手动模式 + 用户设过亮度 + 偏差 > 3%」时写
// 调用频率由调用点限制（每 6 个 tick），避免频繁 fork 与屏幕闪烁
static void bl_tick() {
    int want = bl_want_pct();
    if (!bl_dir()) return;
    int am = bl_auto_mode_cached();
    // 无条件刷新状态文件（每 60 秒一次，dump 靠它零 fork 读状态）
    bl_save_state(am < 0 ? -1 : am, want, bl_cur_raw());
    if (want <= 0) return;
    // 自动亮度开着时不动它（用户显式选了自动，就不该被覆盖）
    if (am == 1) return;
    int cur = bl_cur_raw();
    if (cur < 0) return;
    int target = (int)((long long)g_bl_max * want / 100);
    if (target < 1) target = 1;
    int tol = g_bl_max / 33;      // ≈3%
    if (tol < 1) tol = 1;
    if (cur > target + tol || cur < target - tol) {
        bl_set_pct(want);
        logf("亮度纠正: %d -> %d (目标 %d%%)", cur, target, want);
    }
}

// ============ 充电守护 ============

static bool is_charging() {
    int online = 0;
    if (read_int(N_USB_ONLINE, &online) && online == 1) return true;
    std::string st;
    if (read_str(N_BATT_STAT, st)) {
        if (st == "Charging" || st == "Full") return true;
    }
    return false;
}

// 温度墙运行时状态（全部内存态）。
// 不能拿「读 sysfs 当前值」当反馈来计算目标：charger 驱动写后不一定立即回读一致，
// 一秒钟内读到旧值就会让目标值在 6.6A / 11A 之间反复横跳（实测出现过拉锯）。
// ⚠ 声明必须早于 charge_tick()：cheap_tick 里要用 g_wall_tier 判断是否维持
//   wired_thermal_remove=1，late 声明会导致编译期「未声明标识符」。
static int  g_wall_tier     = 0;   // 当前生效档位
static long g_wall_push_at  = 0;   // 超阈值计时起点（0=未计时）
static long g_wall_relax_at = 0;   // 回落计时起点（0=未计时）
static int  g_wall_last     = -1;  // 上次写入的电流值

static void charge_tick() {
    // 只在充电时纠正限流档
    int cur = -1;
    if (read_int(N_CTRL_LIMIT, &cur)) {
        if (cur != 0) write_int(N_CTRL_LIMIT, 0);
    }
    // fake_temp 保持（驱动可能重置）
    int ft = 0;
    if (read_int(N_FAKE_TEMP, &ft)) {
        if (ft != DEF_TEMP) write_int(N_FAKE_TEMP, DEF_TEMP);
    }
    // OS4：wired_thermal_remove 可能被充电 IC 固件或 mi_thermald 重置为 0，
    // 每轮自愈回 1（幂等写，值已为 1 时不重复写，避免无谓 sysfs 写）。
    // 温度墙要降档时由 wall_tick 负责临时置 0，这里不能无条件覆盖 ——
    // 因此只在「温度墙当前处于 0 档（不限流）」时才维持 removal=1。
    if (g_prof.thermal_ok && g_wall_tier == 0) {
        int rm = -1;
        if (read_int(N_THERMAL_RM, &rm) && rm != 1)
            write_int(N_THERMAL_RM, 1);
    }
}

// ============ 充电温度墙 ============
// 背景：系统自带充电温度墙在 [CLASS0-SIC-BAT]（/data/vendor/thermal/decrypt.txt），
// 但它只是 mi_thermald 解析加密 thermal-map.conf 后的只读快照，改文件不生效。
// 因此由本模块自己实现闭环：读电池真实温度 → 按档位写 wired_chg_curr 限制充电电流。
//
// 为什么用 wired_chg_curr：它是 mi_thermald 的 thermal_fcc_override 目标节点
// （thermald-devices.conf 里 def_target:22000000、select_higher:0），
// 写入小值即被系统当作电流上限采纳，实测写 3000000 可稳定保持不回退。
//
// ============ 电池真实温度源（语义严格）============
//
// 节点语义（实测 M098FE / HyperOS3）：
//   batt_therm  独立 NTC，fake_temp 不改写它 → **唯一可信的真实电芯温度**
//   battery     power_supply 官方口，驱动值被 fake_temp 直接改写 → 伪装后恒 35℃
//   batt_temp   thermal zone，同样被 fake_temp 改写        → 伪装后恒 35℃
//
// 因此：
//   · 首选永远是 batt_therm
//   · battery / batt_temp **只能作为"该节点不存在"时的最后兜底**，
//     并且必须显式告警——它们在有 fake_temp 的机型上给的是假值，
//     用它做温度墙判断会导致「电池烧到 60℃ 也不触发」。
//
// 之前把三者平铺成候选表是错的：任一节点读失败就会静默落到假值上，
// 表面看有数、实际是 35℃，属于"读到了错的东西"而非"读不到"。
// （BATT_TRUE_TYPES / BATT_FB_TYPES 已在上方语义分组区定义，这里不重复定义）

// 读取电池真实温度（0.001℃）。
//
// 取数顺序严格按语义：
//   ① batt_therm（唯一可信）—— daemon 内用持久 fd，绕过 bind mount 取实时真值
//   ② batt_therm 的新鲜快照 —— 供 dump 等独立进程跨进程读取（120s 内有效）
//   ③ 都没有则返回 0，**绝不回退到 battery/batt_temp**
// 返回 0 时上层（wall_tick）会跳过控制，而不是拿假值误判。
static int wall_real_batt_temp() {
    long z = zone_by_type("batt_therm");

    // ① 持久 fd 实时值（daemon 内有效，最准确）
    if (z >= 0) {
        int v = read_live_real(z);
        if (v > 0) return v;
    }
    // ② 新鲜快照（dump 等独立进程）
    if (z >= 0) {
        char p[256];
        snprintf(p, sizeof(p), "%s/thermal_zone%ld", REAL_DIR, z);
        // 必须用 snapshot_fresh() 的三重判据，不能再用 time(nullptr)-st_mtime：
        // 这是安全关键路径（温度墙据它决定限流档位），
        // 系统时间跳回过去时 age 变负 → 快照被丢弃 → 返回 0 → 温度墙静默停用。
        if (snapshot_fresh()) {
            int v = 0;
            if (read_int(p, &v) && v > 0) return v;
        }
    }
    // ③ 明确返回 0：宁可不控，也不用被 fake_temp 改写的假值控
    return 0;
}

// ============ 温度墙档位 ============
//
// 语义（用户明确要求）：
//   **51℃ 之前完全不限流**，到阈值才开始逐步压，目标是别让它越过阈值。
// 所以档 0 覆盖 wall_c 之前全部区间，不设「提前介入」——
// 之前那版 wall_c-3℃ 起就降到 92%，等于没到 51 就开始限，与原脚本行为不符。

// 档位电流比例（index 越大，限流越狠）
static const int WALL_PCT[] = { 100, 72, 60, 48, 36, 26, 16, 8 };
static const int WALL_TIER_N = (int)(sizeof(WALL_PCT) / sizeof(WALL_PCT[0]));

// 时间积分：温度在阈值以上持续不降 → 强制加档。
// 必须存在，否则会出现「某个电流恰好热平衡在阈值上方」的僵局：
// 纯比例算出的档位让它既不上也不下，永远压不回阈值。
static const int WALL_PUSH_SEC  = 20;

// 恢复温度：绝对 40℃（用户要求「降到 40 度左右就恢复」）。
// 不用相对阈值（wall_c-1℃=50℃），那等于温度稍降就放开，立刻反弹。
static const int WALL_RELAX_C   = 40000;
static const int WALL_RELAX_SEC = 10;   // 需在恢复温度以下稳定这么久

// 由温度算「当前至少该到的档位」。返回 0 表示不限流。
static int wall_tier_from_temp(int batt_c, int wall_c) {
    if (batt_c < wall_c) return 0;             // 阈值以下：绝不限流
    int t = 1 + (batt_c - wall_c) / 500;       // 阈值处 72%，每 +0.5℃ 加一档
    if (t > WALL_TIER_N - 1) t = WALL_TIER_N - 1;
    return t;
}

// 温度墙运行时状态已在 charge_tick() 之前声明（需要被 charge_tick 引用）

// 每轮调用：充电时执行温度墙判定，必要时写上限；降温后恢复
// 配置从 wall.conf 读取（"启用 阈值x10" 两字段），WebUI 改完即时生效
// 温度墙档位 0 = 不限流，此时应解除 IC 级温度限流（OS4）；
// 档位 > 0 = 正在限流，必须让 IC 级限流也生效，否则软件限流被硬件绕过。
// OS3 无此节点（thermal_ok=0）→ 全部空操作。
static void wall_sync_thermal_rm(int tier) {
    if (!g_prof.thermal_ok) return;
    int want = (tier == 0) ? 1 : 0;
    int cur = -1;
    if (read_int(N_THERMAL_RM, &cur) && cur == want) return;   // 幂等，不重复写
    if (write_int(N_THERMAL_RM, want))
        logf("充电温度限流: %s (wired_thermal_remove=%d)", want ? "解除" : "恢复", want);
    else
        logf("警告：写 wired_thermal_remove=%d 失败", want);
}

static void wall_tick() {
    // 读配置：enabled wall_c [force_trust]
    int enabled = 1, wall_c = 51000, force_trust = 0;
    {
        char p[192];
        snprintf(p, sizeof(p), "%s/wall.conf", WORK_DIR);
        FILE *fp = fopen(p, "r");
        if (fp) {
            // 第三字段可选：老版本 wall.conf 只有两个字段，必须容错不能重置默认
            int n = fscanf(fp, "%d %d %d", &enabled, &wall_c, &force_trust);
            if (n < 2) { enabled = 1; wall_c = 51000; force_trust = 0; }
            if (n < 3) force_trust = 0;
            fclose(fp);
        }
    }

    int cap = DEF_WIRED_CURR;
    int wc = 0;
    read_int(N_WIRED_CURR, &wc);

    if (!enabled || wall_c <= 0) {
        // 温度墙关闭：若曾被压低，恢复满额
        if (wc > 0 && wc < cap) {
            write_int(N_WIRED_CURR, cap);
            logf("温度墙关闭，恢复充电电流 %d", cap);
        }
        g_wall_tier = 0; g_wall_push_at = 0; g_wall_relax_at = 0; g_wall_last = -1;
        wall_sync_thermal_rm(0);
        return;
    }

    // 可信性保护：必须在读温度之前判定。
    // 为什么顺序重要：没有独立 NTC 的机型上 wall_real_batt_temp() 返回 0，
    // 若把检查放在它之后就会被"读不到温度"提前 return 掉，
    // 用户只看到"温度墙: 开启"却完全不知道它其实不工作 —— 属于诊断信息缺失。
    int trusted = 1;
    {
        static int trusted_cached = -2;
        if (trusted_cached == -2) {
            trusted_cached = load_profile() ? (g_prof.batt_trusted ? 1 : 0) : 1;
            if (trusted_cached == 0) {
                logf("温度墙停用：batt_trusted=0（无独立 NTC，电池温度不可信）");
                logf("温度墙停用原因：profile 中 batt_trusted=0。"
                     "本机没有不受 fake_temp 影响的电池温度节点，"
                     "伪装后读到的恒为设定值，无法用于限流判断。"
                     "如需强制启用可执行: temp_charge_boost wall trust");
            } else if (trusted_cached == 1 && g_prof.batt_zone >= 0) {
                logf("温度墙数据源：zone%ld (%s)，实测不受 fake_temp 影响",
                     g_prof.batt_zone, g_prof.batt_type.c_str());
            }
        }
        trusted = trusted_cached;
    }

    // 用户在 wall.conf 里显式要求强制启用时，跳过可信性拦截（用户自担风险，
    // 但必须留日志，避免以后又忘了为什么"开了却不工作"）
    if (force_trust && trusted == 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            logf("温度墙：用户强制启用（wall trust），将使用被 fake_temp 改写的节点判断，"
                 "可能永远不会触发限流");
        }
        trusted = 1;
    }

    int bt = wall_real_batt_temp();

    if (trusted == 0) {
        // 不可信：拒绝限流，并把可能残留的限流恢复满额
        if (wc > 0 && wc < cap) {
            write_int(N_WIRED_CURR, cap);
            logf("温度墙不可信，恢复充电电流 %d", cap);
        }
        g_wall_tier = 0; g_wall_push_at = 0; g_wall_relax_at = 0; g_wall_last = -1;
        wall_sync_thermal_rm(0);
        return;
    }

    if (bt <= 0) return;                       // 读不到真实温度就不要乱写

    // ⚠ 时间基准统一用单调时钟（秒）。
    //   原来用 time(nullptr)：系统时间跳变会让 g_wall_push_at / relax_at 算出
    //   负数或巨大的间隔 —— 要么积分项永久不触发（限流失控），要么瞬间跳档。
    //   温度墙是安全相关逻辑，时间基准不能受用户改时间影响。
    long now = mono_ms() / 1000L;

    int need = wall_tier_from_temp(bt, wall_c);

    // 积分项：温度在阈值以上持续不降 → 定时强制加档
    if (bt >= wall_c) {
        if (g_wall_push_at == 0) g_wall_push_at = now;
        if (now - g_wall_push_at >= WALL_PUSH_SEC) {
            int t = (g_wall_tier > need ? g_wall_tier : need) + 1;
            if (t > WALL_TIER_N - 1) t = WALL_TIER_N - 1;
            if (t > need) {
                logf("温度墙: 电池 %d.%d℃ 持续 %ds 未降，加档至 %d%%",
                     bt / 1000, (bt % 1000) / 100, WALL_PUSH_SEC, WALL_PCT[t]);
            }
            need = t;
            g_wall_push_at = now;
        }
    } else {
        g_wall_push_at = 0;                    // 温度回落，积分清零
    }

    // 恢复温度：默认 40℃，但必须低于阈值（用户把阈值设到 42℃ 以下时自动让路）
    int relax_c = WALL_RELAX_C;
    if (relax_c > wall_c - 3000) relax_c = wall_c - 3000;
    if (relax_c < 30000) relax_c = 30000;      // 兜底下限

    // 档位状态机：
    //   升档 —— 立即生效（温度往上走必须马上压住）
    //   放档 —— 必须降到 relax_c 以下并稳定 WALL_RELAX_SEC 秒，
    //           然后一次性回到 need（通常是 0 = 满额），不逐档磨。
    int tier = g_wall_tier;
    if (need >= tier) {
        tier = need;                 // 升档或持平：直接跟随
        g_wall_relax_at = 0;
    } else if (bt <= relax_c) {
        // 已降到恢复温度以下：计时，稳定够久才允许放档
        if (g_wall_relax_at == 0) g_wall_relax_at = now;
        if (now - g_wall_relax_at >= WALL_RELAX_SEC) {
            tier = need;             // 一次性放到应有档位
            g_wall_relax_at = 0;
        }
    } else {
        // 温度仍在恢复点之上：保持当前限流，并清计时
        g_wall_relax_at = 0;
    }
    g_wall_tier = tier;

    // 档位 0 = 不限流，恢复满额。
    // 用 sysfs 当前值判断（而不是 g_wall_last）：恢复满额是幂等操作，
    // 而且能自愈「外部把值改小了」以及「上一轮 daemon 留下的低值」两种情况。
    // 写成 cap 之后 wc 会等于 cap，下一轮自然不再写，不会每秒刷写。
    if (tier == 0) {
        if (wc > 0 && wc < cap) {
            write_int(N_WIRED_CURR, cap);
            logf("电池 %d.%d℃ 已回落，恢复充电电流 %d",
                 bt / 1000, (bt % 1000) / 100, cap);
        }
        g_wall_last = -1;
        wall_sync_thermal_rm(0);
        return;
    }

    int want = cap * WALL_PCT[tier] / 100;
    if (want < WALL_CAP_CURR) want = WALL_CAP_CURR;   // 不低于 3A，避免触发涓流停充

    // 限流生效时，IC 级温度限流必须恢复，否则硬件会绕过软件限流继续拉满电流
    wall_sync_thermal_rm(tier);

    // 与「上次写入值」比较，而非与 sysfs 当前值比较，避免反馈回路。
    int deadband = cap * 2 / 100;
    if (g_wall_last < 0 || abs(g_wall_last - want) > deadband) {
        write_int(N_WIRED_CURR, want);
        logf("温度墙: 电池 %d.%d℃ (阈值 %d.%d℃)，充电电流上限 %d -> %d (%d%%，第 %d 档)",
             bt / 1000, (bt % 1000) / 100,
             wall_c / 1000, (wall_c % 1000) / 100,
             g_wall_last < 0 ? wc : g_wall_last, want, WALL_PCT[tier], tier);
        g_wall_last = want;
    }
}

// ============ daemon ============

static void run_daemon() {
    // 降低被杀的优先级
    int fd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) { write(fd, "-1000", 5); close(fd); }

    // pid 文件
    {
        char path[128];
        snprintf(path, sizeof(path), "%s/daemon.pid", WORK_DIR);
        ensure_dir(WORK_DIR);
        FILE *fp = fopen(path, "w");
        if (fp) { fprintf(fp, "%d\n", getpid()); fclose(fp); }
    }
    // 关键：先清掉可能存在的旧挂载，确保后续 open 拿到的是真实 inode
    restore_temp();
    // 现在路径是真实的，打开持久 fd（此后挂载不影响这些 fd）
    open_real_fds();
    apply_temp(DEF_TEMP);
    logf("守护进程启动 pid=%d", getpid());
    flush_real_values();

    bool last_chg = false;
    int tick = 0;
    long last_flush = 0;
    // 亮度：不再自动纠正。亮度完全由用户自己调（WebUI 或 bright 命令），
    // daemon 不参与，避免与用户手动调节冲突导致屏幕忽明忽暗。
    //
    // 轮询结构：温度墙每秒判定（热路径，只读一个 temp + 可能写一个节点），
    // 真实温度快照每 FLUSH_INTERVAL 秒刷一次（要读写约 60 个文件，不能每秒做）。
    for (;;) {
        bool chg = is_charging();
        if (chg != last_chg) {
            logf("充电状态变化: %s", chg ? "开始充电" : "停止充电");
            last_chg = chg;
        }

        // 热路径：温度墙每轮都跑，保证 1 秒级响应
        if (chg) charge_tick();
        wall_tick();

        // 冷路径：快照按固定间隔刷新
        // ⚠ 必须用单调时钟：原来用 time(nullptr) 做差，一旦系统时间倒退
        //   （开机早期 RTC 未同步、手动改时间），now - last_flush 变负数，
        //   条件永远不成立 —— 快照从此再也不刷新，WebUI 真实温度全部停摆。
        //   实测踩过：日志里时间从 10-28 跳回 01-13 后，快照就再没更新过。
        long mono = mono_ms();
        if (mono - last_flush >= (long)FLUSH_INTERVAL * 1000L) {
            flush_real_values();
            last_flush = mono;
        }

        sleep(chg ? POLL_CHARGING : POLL_IDLE);
        tick++;

    }
}

// ============ status ============

static void show_status() {
    int cnt = 0;
    DIR *d = opendir(ZONE_ROOT);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
            std::string tgt = std::string(ZONE_ROOT) + "/" + e->d_name + "/temp";
            if (is_mounted(tgt.c_str())) cnt++;
        }
        closedir(d);
    }
    printf("已伪装节点: %d\n", cnt);

    const struct { const char *p; const char *n; } items[] = {
        { N_FAKE_TEMP,  "fake_temp    " },
        { N_BATT_TEMP,  "battery/temp " },
        { N_CTRL_LIMIT, "ctrl_limit   " },
        { N_PPS_PTF,    "fake_pps_ptf " },
    };
    for (auto &it : items) {
        int v = 0;
        if (read_int(it.p, &v)) printf("  %s = %d\n", it.n, v);
        else                      printf("  %s = (读失败)\n", it.n);
    }
    printf("  充电中       = %s\n", is_charging() ? "是" : "否");
}

// 读取 REAL_DIR 中记录的真实值（挂载前快照）
static int read_real(const char *zone) {
    char p[256];
    snprintf(p, sizeof(p), "%s/%s", REAL_DIR, zone);
    int v = 0;
    if (!read_int(p, &v)) return 0;
    return v;
}

// ============ 持久 fd 读取真实温度（绕过 bind mount）============
// 原理：mount --bind 只改变路径解析，已打开的 fd 仍指向原始 inode，
// 所以挂载前先 open 保存 fd，之后 pread 每次都能拿到实时真实值。
// 注意：fd 只在持有它的进程内有效，daemon 需把实时值写入 REAL_DIR 供 WebUI 读取。

#include <map>
#include <ctime>
static std::map<long, int> g_real_fd;   // zone 编号 -> fd

// ============ 快照新鲜度判定（不依赖系统时间）============
//
// 为什么不能用 time(nullptr) - st_mtime：
//   实测踩过 —— 设备重启后 RTC 未同步，系统时间从 2026-10 跳回 1970-01，
//   快照文件 mtime 变成 1970，算出的 age 是几十年（甚至负数），
//   导致所有快照被判定为"陈旧"而全部丢弃，WEBUI 真实温度全变 0.3℃。
//   这类时钟跳变在手机上是常态（开机早期、时区/手动改时间、NTP 回拨），
//   拿 wall clock 做新鲜度判定本身就是错的。
//
// 现在的判据（三重，任一不满足即视为陈旧）：
//   ① boot_id 必须等于当前开机 —— 排除"上次开机写的快照"
//   ② 写入时刻用 CLOCK_MONOTONIC 记录 —— 单调递增，不受改时间影响
//   ③ 单调时钟差 <= 120 秒 —— 排除"本次开机但 daemon 早已停掉"
//
// 侧车文件格式：REAL_DIR/.meta 内容为 "<boot_id> <monotonic_ms>"
static const char *current_boot_id() {
    // 一次开机内不会变，缓存住避免反复 open
    static char cached[64] = {0};
    if (cached[0]) return cached;
    int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    ssize_t n = read(fd, cached, sizeof(cached) - 1);
    close(fd);
    if (n <= 0) { cached[0] = 0; return ""; }
    cached[n] = 0;
    // 去掉尾部换行，避免比对时差一个 \n
    for (int i = 0; cached[i]; i++)
        if (cached[i] == '\n' || cached[i] == '\r') { cached[i] = 0; break; }
    return cached;
}

// 写快照元信息（每次 flush 调用一次，不按 zone 重复写）
static void write_snapshot_meta() {
    const char *bid = current_boot_id();
    if (!bid[0]) return;               // 拿不到 boot_id 就不写，read 侧会视为不可用
    char p[256];
    snprintf(p, sizeof(p), "%s/.meta", REAL_DIR);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    char b[128];
    int k = snprintf(b, sizeof(b), "%s %ld\n", bid, mono_ms());
    ssize_t w = write(fd, b, k); (void)w;
    close(fd);
}

// 快照是否可用（新鲜且属于本次开机）
static bool snapshot_fresh() {
    const char *bid = current_boot_id();
    if (!bid[0]) return false;
    char p[256];
    snprintf(p, sizeof(p), "%s/.meta", REAL_DIR);
    FILE *fp = fopen(p, "r");
    if (!fp) return false;
    char sb[64] = {0};
    long written = 0;
    int got = fscanf(fp, "%63s %ld", sb, &written);
    fclose(fp);
    if (got != 2) return false;
    if (strcmp(sb, bid) != 0) return false;          // 上次开机写的，丢弃
    long age = mono_ms() - written;
    if (age < 0) return false;                        // 单调时钟倒退（理论上不会）
    if (age > 120000L) return false;                  // 超过 120 秒未刷新
    return true;
}

static void open_real_fds() {
    DIR *d = opendir(ZONE_ROOT);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
        long zn = atol(e->d_name + 12);
        if (g_real_fd.count(zn)) continue;
        char p[256];
        snprintf(p, sizeof(p), "%s/%s/temp", ZONE_ROOT, e->d_name);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) g_real_fd[zn] = fd;
    }
    closedir(d);
}

static void close_real_fds() {
    for (auto &kv : g_real_fd) close(kv.second);
    g_real_fd.clear();
}

// 通过持久 fd 读实时真实温度
static int read_live_real(long zn) {
    auto it = g_real_fd.find(zn);
    if (it == g_real_fd.end()) return 0;
    char b[32];
    ssize_t n = pread(it->second, b, sizeof(b) - 1, 0);
    if (n <= 0) return 0;
    b[n] = 0;
    return atoi(b);
}

// 读真实温度（三级回退）：
// ① 持久 fd 实时值（仅 daemon 进程内有效，最准确）
// ② REAL_DIR 快照（daemon 写，供 dump 等独立进程跨进程读取；按 boot_id + 单调时钟判新鲜）
// ③ 直接读 sysfs —— **仅当该节点没有被本模块伪装时**
//
// 为什么 ③ 必须加"未伪装"条件：
//   实测踩过 —— 快照被判陈旧被丢弃后，回退读 sysfs 拿到的是 bind mount 之后的
//   伪装值（350），却被当成"真实温度"显示，WebUI 上真实温度全是 0.3℃。
//   这比返回 0 更糟：0 至少能看出"读不到"，假值会被当真。
//   所以节点若已挂载我们的假值文件，就不能拿 sysfs 当真实值，直接返回 0。
static int read_best(long zn) {
    int v = read_live_real(zn);
    if (v > 0) return v;

    char p[256];
    if (snapshot_fresh()) {
        snprintf(p, sizeof(p), "%s/thermal_zone%ld", REAL_DIR, zn);
        if (read_int(p, &v) && v > 0) return v;
    }

    // ③ sysfs：只有确认未被伪装才可信
    snprintf(p, sizeof(p), "%s/thermal_zone%ld/temp", ZONE_ROOT, zn);
    if (is_mounted(p)) return 0;        // 已被本模块接管，读到的是假值，不能当真实值
    if (read_int(p, &v) && v > 0) return v;
    return 0;
}

// CPU 每核温度：按 type 的 cpu-<cluster>-<core>-<sensor> 聚合，
// 同一核的多个传感器取最大值。返回核数，out 为每核温度（0.001℃）。
// 这样输出的是「8 个核」而不是「16 个传感器」，符合直觉。
static int cpu_core_temps(int *out, int maxn) {
    long zs[64];
    int  keys[64];
    int  zn = 0;
    DIR *d = opendir(ZONE_ROOT);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && zn < 64) {
        if (strncmp(e->d_name, "thermal_zone", 12) != 0) continue;
        std::string tp = std::string(ZONE_ROOT) + "/" + e->d_name + "/type";
        std::string t;
        if (!read_str(tp.c_str(), t)) continue;
        int a = -1, b = -1;
        // 只接受 cpu-N-M-* 形式，自动排除 cpu-hw-trip-N / cpu_therm
        if (sscanf(t.c_str(), "cpu-%d-%d-", &a, &b) != 2) continue;
        keys[zn] = a * 100 + b;
        zs[zn]   = atol(e->d_name + 12);
        zn++;
    }
    closedir(d);

    // 去重 + 按 (cluster, core) 升序
    int uk[64], un = 0;
    for (int i = 0; i < zn; i++) {
        bool dup = false;
        for (int k = 0; k < un; k++) if (uk[k] == keys[i]) { dup = true; break; }
        if (!dup && un < 64) uk[un++] = keys[i];
    }
    for (int i = 0; i < un; i++)
        for (int k = i + 1; k < un; k++)
            if (uk[k] < uk[i]) { int t = uk[i]; uk[i] = uk[k]; uk[k] = t; }

    int n = 0;
    for (int i = 0; i < un && n < maxn; i++) {
        int mx = 0;
        for (int k = 0; k < zn; k++) {
            if (keys[k] != uk[i]) continue;
            int v = read_best(zs[k]);
            if (v > mx) mx = v;
        }
        out[n++] = mx;
    }
    return n;
}

// 把所有关键 zone 的实时真实值写入 REAL_DIR（供 WebUI 跨进程读取）
// 缓冲区按最多的场景预留：CPU 8 + GPU 11 + 电池/充电若干
static void flush_real_values() {
    long zones[64];
    int n = 0;

    // CPU：cpu-0-x / cpu-1-x（真实核心温度，不带 hw-trip）
    long tmp[64];
    int m = zones_by_prefix("cpu-0-", tmp, 64);
    for (int i = 0; i < m && n < 64; i++) zones[n++] = tmp[i];
    m = zones_by_prefix("cpu-1-", tmp, 64);
    for (int i = 0; i < m && n < 64; i++) zones[n++] = tmp[i];
    // GPU：gpuss-*
    m = zones_by_prefix("gpuss-", tmp, 64);
    for (int i = 0; i < m && n < 64; i++) zones[n++] = tmp[i];
    // 电池 / 充电口 / 主板（按 type 名查，不硬编码编号）
    static const char *SINGLE[] = {
        "battery", "batt_temp", "batt_therm",
        "charger_therm", "conn_therm", "conn_therm2",
        "xoagg_therm", "cpullc-0-0", "X7_therm",
        nullptr
    };
    for (int i = 0; SINGLE[i]; i++) {
        long z = zone_by_type(SINGLE[i]);
        if (z >= 0 && n < 64) zones[n++] = z;
    }

    int last_written = 0;

    for (int i = 0; i < n; i++) {
        long zn = zones[i];
        int v = read_live_real(zn);
        if (v <= 0) continue;
        char p[256];
        snprintf(p, sizeof(p), "%s/thermal_zone%ld", REAL_DIR, zn);
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) continue;
        char b[16];
        int k = snprintf(b, sizeof(b), "%d\n", v);
        ssize_t w = write(fd, b, k); (void)w;
        close(fd);
        if (w > 0) last_written++;
    }

    // 关键：写完数据必须同步更新元信息，否则 read_best() 的 snapshot_fresh()
    // 会因为找不到 / 过期的 .meta 而把上面刚写的快照全部判为陈旧丢弃。
    // 有有效数据才写 meta：一个都没写到就说明 daemon 拿不到真实值（fd 没打开），
    // 此时不该把 meta 刷新成"新鲜"，否则会把空快照当成有效数据。
    if (last_written > 0) write_snapshot_meta();
}

// ============ dump（WebUI 用）============

static void dump_json() {
    // 已伪装节点数 = 挂了 mount 的 thermal_zone 唯一目标数
    // 一次性扫描 mountinfo 统计（原来对 97 个 zone 各调一次 is_mounted，共 97 次 fopen）
    int layers_total = count_all_thermal_layers();
    int cnt = 0;
    {
        // 用 layers > 0 的 zone 数作为"已伪装节点数"
        FILE *fp = fopen("/proc/self/mountinfo", "r");
        if (fp) {
            char line[4096];
            char seen[128][32];
            int seen_n = 0;
            const size_t pre = strlen(ZONE_ROOT);
            while (fgets(line, sizeof(line), fp)) {
                int f = 0;
                char *p = line;
                while (*p && f < 4) { if (*p == ' ') f++; p++; }
                if (f != 4) continue;
                char *end = strchr(p, ' ');
                if (!end) continue;
                *end = 0;
                if (strncmp(p, ZONE_ROOT, pre) != 0) continue;
                // 提取 thermal_zoneN 部分做去重
                char *zn = strstr(p, "/thermal_zone");
                if (!zn) continue;
                zn++;  // 跳过 '/'
                char name[32] = {0};
                size_t k = 0;
                while (zn[k] && zn[k] != '/' && k < sizeof(name) - 1) { name[k] = zn[k]; k++; }
                if (!k) continue;
                bool dup = false;
                for (int i = 0; i < seen_n; i++) {
                    if (strcmp(seen[i], name) == 0) { dup = true; break; }
                }
                if (!dup && seen_n < 128) {
                    strncpy(seen[seen_n], name, sizeof(seen[0]) - 1);
                    seen[seen_n][sizeof(seen[0]) - 1] = 0;
                    seen_n++;
                    cnt++;
                }
            }
            fclose(fp);
        }
    }

    int ft = 0, bt = 0, cl = -1, pp = -1;
    read_int(N_FAKE_TEMP, &ft);
    read_int(N_BATT_TEMP, &bt);
    read_int(N_CTRL_LIMIT, &cl);
    read_int(N_PPS_PTF, &pp);

    // 真实温度：优先读电荷泵自带 NTC（cp_master/batt_temp，单位 0.01℃）
    // 但电荷泵停工时该节点会返回陈旧值（实测卡死 6400 = 64℃，恰是安全上限），
    // 判据：ibus 接近 0（未在充放电）或值明显不合理时，回退到独立 NTC。
    //
    // ⚠ 回退目标必须是"不受 fake_temp 影响"的节点（batt_therm），
    //   不能用 charger_therm —— 后者也在伪装范围内，回退过去等于换个假值。
    //   实测踩过：回退到 charger_therm 后 real_batt_temp 直接显示 35.0℃（伪装值）。
    int real_bt = 0;
    {
        int cp_raw = 0;
        read_int("/sys/class/xm_power/cp_master/batt_temp", &cp_raw);
        int cp_ibus = 0;
        read_int("/sys/class/xm_power/cp_master/ibus", &cp_ibus);
        bool cp_idle = (cp_ibus <= 10);

        // cp_master 单位是 0.01℃，有效范围 10.00~99.99℃（1000~9999）
        // 6400 正是"没充电时驱动返回的默认/陈旧值"，必须排除
        bool cp_valid = (cp_raw >= 1000 && cp_raw <= 9999) && !cp_idle;
        if (cp_valid) {
            real_bt = cp_raw;
        } else {
            // 回退到实测确认的独立 NTC
            long z = zone_by_type("batt_therm");
            if (z < 0 && load_profile() && g_prof.batt_trusted)
                z = g_prof.batt_zone;
            int v = (z >= 0) ? read_best(z) : 0;
            if (v > 0) real_bt = v / 10;   // 0.001℃ -> 0.01℃，与 cp_master 同刻度
        }
    }

    // === 关键指标：真实温度（全部按 type 名查找，不硬编码编号）===
    // CPU：按 cpu-<cluster>-<core> 聚合为每核最高温，输出 8 个核
    int cpu_vals[32];
    int cpu_cnt = cpu_core_temps(cpu_vals, 32);
    // GPU：gpuss-* 取最高温（用户要求只显示一个 GPU 温度）
    int gpu_real = 0;
    {
        long tmp[32];
        int m = zones_by_prefix("gpuss-", tmp, 32);
        for (int i = 0; i < m; i++) {
            int v = read_best(tmp[i]);
            if (v > gpu_real) gpu_real = v;
        }
    }
    int cpu_therm_real = 0;
    {
        long z = zone_by_type("cpu_therm");
        if (z >= 0) cpu_therm_real = read_best(z);
    }
    int gpu_kgsl = 0;
    read_int("/sys/class/kgsl/kgsl-3d0/temp", &gpu_kgsl);
    // 电池 / 充电口：按 type 名查找
    // batt_real 取 batt_therm（独立 NTC，不受 fake_temp 影响）
    int batt_real = 0, chg_real = 0, conn_real = 0;
    {
        long z = zone_by_type("batt_therm");
        if (z >= 0) batt_real = read_best(z);
        z = zone_by_type("charger_therm");
        if (z >= 0) chg_real = read_best(z);
        z = zone_by_type("conn_therm");
        if (z >= 0) conn_real = read_best(z);
    }
    // 电池组：batt_therm 是独立 NTC（真实）；battery/batt_temp 会被 fake_temp 改写。
    // 因此 batt_real 取 batt_therm，batt_drv 只作参考。
    int batt_ps = 0, batt_drv = 0, batt_ntc = 0;
    read_int(N_BATT_TEMP, &batt_ps);
    {
        long z = zone_by_type("batt_therm");
        if (z >= 0) batt_ntc = read_best(z);
        z = zone_by_type("batt_temp");
        if (z >= 0) batt_drv = read_best(z);
    }

    // 充电功率
    int usb_v = 0, usb_i = 0, bat_v = 0, bat_i = 0;
    read_int("/sys/class/power_supply/usb/voltage_now", &usb_v);
    read_int("/sys/class/power_supply/usb/current_now", &usb_i);
    read_int("/sys/class/power_supply/battery/voltage_now", &bat_v);
    read_int("/sys/class/power_supply/battery/current_now", &bat_i);

    int cap = 0;
    read_int("/sys/class/power_supply/battery/capacity", &cap);

    std::string status;
    read_str("/sys/class/power_supply/battery/status", status);

    // 守护进程存活
    // ⚠ 不能只看 /proc/<pid> 是否存在：PID 会被内核复用。
    //   实测踩过 —— daemon 早就死了，pid 文件留着 10038，
    //   而 10038 已被 com.miui.daemon 占用，WebUI 于是显示"守护进程运行中"，
    //   用户完全不知道实际已停摆（真实温度也读不到，全显示 0.3℃）。
    //   判据：/proc/<pid>/cmdline 必须真的指向本模块二进制。
    bool daemon_alive = false;
    {
        char p[128];
        snprintf(p, sizeof(p), "%s/daemon.pid", WORK_DIR);
        int pid = 0;
        if (read_int(p, &pid) && pid > 0) {
            char sp[128];
            snprintf(sp, sizeof(sp), "/proc/%d/cmdline", pid);
            int fd = open(sp, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                char cb[512] = {0};
                ssize_t n = read(fd, cb, sizeof(cb) - 1);
                close(fd);
                if (n > 0) {
                    // cmdline 是 \0 分隔，转成空格方便 strstr
                    for (ssize_t i = 0; i < n; i++) if (cb[i] == '\0') cb[i] = ' ';
                    // 必须同时命中二进制名和 daemon 子命令，防同名进程误判
                    if (strstr(cb, "temp_charge_boost") && strstr(cb, "daemon"))
                        daemon_alive = true;
                }
            }
        }
    }

    printf("{");
    printf("\"mounted\":%d,", cnt);
    // 真实挂载总层数（正常应等于 mounted；若远大于则说明发生叠层）
    // 复用上面已算好的结果，不再重复扫 mountinfo
    printf("\"layers\":%d,", layers_total);
    printf("\"fake_temp\":%d,", ft);
    printf("\"batt_temp\":%d,", bt);
    printf("\"real_batt_temp\":%d,", real_bt);
    printf("\"ctrl_limit\":%d,", cl);
    printf("\"pps_ptf\":%d,", pp);
    printf("\"usb_voltage\":%d,", usb_v);
    printf("\"usb_current\":%d,", usb_i);
    printf("\"bat_voltage\":%d,", bat_v);
    printf("\"bat_current\":%d,", bat_i);
    printf("\"capacity\":%d,", cap);
    printf("\"status\":\"%s\",", status.c_str());
    printf("\"charging\":%s,", is_charging() ? "true" : "false");
    printf("\"daemon_alive\":%s,", daemon_alive ? "true" : "false");
    // 真实温度（单位 0.001℃，除以 1000 得 ℃）
    // CPU 每核数组：cpu_list 按 zone 升序（cpu-0-* 在前，即超大核在前）
    printf("\"cpu_list\":[");
    for (int i = 0; i < cpu_cnt; i++)
        printf("%s%d", i ? "," : "", cpu_vals[i]);
    printf("],");
    int cpu_max = 0;
    for (int i = 0; i < cpu_cnt; i++) if (cpu_vals[i] > cpu_max) cpu_max = cpu_vals[i];
    printf("\"cpu_max\":%d,", cpu_max);
    printf("\"cpu_real\":%d,", cpu_cnt > 0 ? cpu_vals[0] : 0);
    printf("\"cpu_therm_real\":%d,", cpu_therm_real);
    printf("\"gpu_real\":%d,", gpu_real);
    printf("\"gpu_kgsl\":%d,", gpu_kgsl);
    printf("\"batt_real\":%d,", batt_real);
    printf("\"batt_ps\":%d,", batt_ps);
    printf("\"batt_drv\":%d,", batt_drv);
    printf("\"batt_ntc\":%d,", batt_ntc);
    printf("\"chg_real\":%d,", chg_real);
    printf("\"conn_real\":%d,", conn_real);
    // === 充电温度墙状态 ===
    {
        int we = 1, wc2 = 51000;
        char p[192];
        snprintf(p, sizeof(p), "%s/wall.conf", WORK_DIR);
        FILE *fp = fopen(p, "r");
        if (fp) { if (fscanf(fp, "%d %d", &we, &wc2) != 2) { we = 1; wc2 = 51000; } fclose(fp); }
        int wcur = 0;
        read_int(N_WIRED_CURR, &wcur);
        printf("\"wall_on\":%d,", we);
        printf("\"wall_c\":%d,", wc2);
        printf("\"wall_curr\":%d,", wcur);
    }
    // === 屏幕亮度 ===
    // 面板发现失败时 ok=false，WebUI 据此隐藏亮度 tab
    {
        const char *dir = bl_dir();
        if (dir) {
            int cur = bl_cur_raw();
            int st_auto = -1, st_pct = 0, st_raw = 0;
            bool has_st = bl_load_state(&st_auto, &st_pct, &st_raw);
            (void)st_pct; (void)st_raw;
            // 自动模式优先取状态文件（零 fork）；缺失时才落一次 settings get
            int am = has_st ? st_auto : bl_auto_mode_cached();
            printf("\"bl_ok\":true,");
            printf("\"bl_max\":%d,", g_bl_max);
            printf("\"bl_raw\":%d,", cur < 0 ? 0 : cur);
            printf("\"bl_pct\":%d,", bl_pct_of(cur));
            printf("\"bl_want\":%d,", bl_want_pct());
            printf("\"bl_auto\":%d,", am < 0 ? -1 : am);
        } else {
            printf("\"bl_ok\":false,\"bl_max\":0,\"bl_raw\":0,\"bl_pct\":0,\"bl_want\":0,\"bl_auto\":-1,");
        }
    }
    printf("\"fake_val\":%d", ft);
    printf("}\n");
}

// ============ main ============

int main(int argc, char **argv) {
    init_paths();                    // 必须在任何路径访问之前
    const char *cmd = argc > 1 ? argv[1] : "status";

    if (!strcmp(cmd, "daemon"))       run_daemon();
    else if (!strcmp(cmd, "scan"))    do_scan(argc > 2 && !strcmp(argv[2], "-v"));
    else if (!strcmp(cmd, "apply"))   apply_temp(argc > 2 ? atoi(argv[2]) : DEF_TEMP);
    else if (!strcmp(cmd, "restore")) restore_temp();
    else if (!strcmp(cmd, "status"))  show_status();
    else if (!strcmp(cmd, "dump"))    dump_json();
    else if (!strcmp(cmd, "profile")) {
        // profile          → 读取并打印已保存的适配结果
        // profile -v       → 打印并附当前节点清单
        if (!load_profile()) {
            printf("尚未生成 profile.conf，请先执行: %s scan\n", argv[0]);
            return 1;
        }
        printf("适配结果 (%s)\n", g_prof.stamp.c_str());
        printf("  thermal_zone 总数   = %d\n", g_prof.zone_total);
        printf("  可伪装 / 跳过       = %d / %d\n", g_prof.mask_count, g_prof.skip_count);
        printf("  电池真实温度源      = %s\n",
               g_prof.batt_zone >= 0 && g_prof.batt_trusted
                 ? ("zone" + std::to_string(g_prof.batt_zone) + " (" +
                    g_prof.batt_type + ") ✓ 独立NTC/可信").c_str()
                 : "未找到可信独立 NTC —— 温度墙将自动停用");
        if (g_prof.batt_fb_zone >= 0)
            printf("  电池兜底节点        = zone%ld (%s) ⚠ 受 fake_temp 改写，仅作显示\n",
                   g_prof.batt_fb_zone, g_prof.batt_fb_type.c_str());
        printf("  CPU 首传感器        = zone%ld\n", g_prof.cpu_first);
        printf("  GPU 首传感器        = zone%ld\n", g_prof.gpu_first);
        printf("  充电口传感器        = zone%ld (%s)\n", g_prof.conn_zone,
               g_prof.conn_type.empty() ? "未找到" : g_prof.conn_type.c_str());
        printf("  充电控制点          = curr:%s ctrl:%s pps:%s fake:%s\n",
               g_prof.cur_ok  ? "可写" : "缺失", g_prof.ctrl_ok ? "可写" : "缺失",
               g_prof.pps_ok  ? "可写" : "缺失", g_prof.fake_ok ? "可写" : "缺失");
        printf("  屏幕背光            = %s (max=%d)\n",
               g_prof.bl_dir.empty() ? "未找到" : g_prof.bl_dir.c_str(), g_prof.bl_max);
        return 0;
    }
    else if (!strcmp(cmd, "bright")) {
        // bright            → 打印当前亮度状态
        // bright <1-100>    → 设为该百分比
        // bright auto on|off→ 切换自动亮度
        const char *a2 = argc > 2 ? argv[2] : NULL;
        if (!a2) {
            const char *dir = bl_dir();
            if (!dir) { printf("{\"bl_ok\":false}\n"); return 1; }
            int cur = bl_cur_raw();
            int am  = bl_auto_mode();
            printf("{\"bl_ok\":true,\"panel\":\"%s\",\"max\":%d,\"raw\":%d,"
                   "\"pct\":%d,\"want\":%d,\"auto\":%d}\n",
                   dir, g_bl_max, cur < 0 ? 0 : cur, bl_pct_of(cur),
                   bl_want_pct(), am < 0 ? -1 : am);
            return 0;
        }
        if (!strcmp(a2, "auto")) {
            const char *a3 = argc > 3 ? argv[3] : "";
            bool on = (strcmp(a3, "on") == 0 || strcmp(a3, "1") == 0);
            if (!bl_set_auto(on)) { printf("{\"ok\":false,\"why\":\"no_settings\"}\n"); return 1; }
            logf("自动亮度: %s", on ? "开启" : "关闭");
            printf("{\"ok\":true,\"auto\":%d}\n", on ? 1 : 0);
            return 0;
        }
        int pct = atoi(a2);
        if (pct < 1 || pct > 100) { printf("{\"ok\":false,\"why\":\"range\"}\n"); return 1; }
        if (!bl_dir()) { printf("{\"ok\":false,\"why\":\"no_panel\"}\n"); return 1; }
        bool ok = bl_set_pct(pct);
        logf("亮度设为 %d%%", pct);
        printf("{\"ok\":%s,\"pct\":%d,\"raw\":%d}\n", ok ? "true" : "false", pct, bl_cur_raw());
        return ok ? 0 : 1;
    }
    else if (!strcmp(cmd, "wall")) {
        // wall                → 打印温度墙状态
        // wall <30-60>        → 设阈值（℃）
        // wall on|off         → 开关
        // wall trust [on|off] → 无视 batt_trusted=0 强制启用（默认 on）
        const char *a2 = argc > 2 ? argv[2] : NULL;
        char p[192];
        snprintf(p, sizeof(p), "%s/wall.conf", WORK_DIR);
        int we = 1, wc = 51000, wf = 0;
        {
            FILE *fp = fopen(p, "r");
            if (fp) {
                int n = fscanf(fp, "%d %d %d", &we, &wc, &wf);
                if (n < 2) { we = 1; wc = 51000; }
                if (n < 3) wf = 0;
                fclose(fp);
            }
        }
        if (a2 && !strcmp(a2, "trust")) {
            const char *a3 = argc > 3 ? argv[3] : "on";
            wf = (strcmp(a3, "off") == 0 || strcmp(a3, "0") == 0) ? 0 : 1;
            ensure_dir(WORK_DIR);
            FILE *fp = fopen(p, "w");
            if (fp) { fprintf(fp, "%d %d %d\n", we, wc, wf); fclose(fp); }
            logf("温度墙强制启用: %s", wf ? "开（忽略 batt_trusted）" : "关");
            printf("{\"ok\":true,\"wall_force\":%d}\n", wf);
            return 0;
        }
        if (a2) {
            if (!strcmp(a2, "on"))       we = 1;
            else if (!strcmp(a2, "off")) we = 0;
            else {
                int c = atoi(a2);
                if (c < 30 || c > 60) { printf("{\"ok\":false,\"why\":\"range\"}\n"); return 1; }
                wc = c * 1000;
                we = 1;
            }
            ensure_dir(WORK_DIR);
            FILE *fp = fopen(p, "w");
            if (fp) { fprintf(fp, "%d %d %d\n", we, wc, wf); fclose(fp); }
            logf("温度墙: %s, 阈值 %d℃", we ? "开启" : "关闭", wc / 1000);
            // 关闭时立刻恢复电流，不等下一轮
            if (!we) {
                int cur = 0;
                if (read_int(N_WIRED_CURR, &cur) && cur > 0 && cur < DEF_WIRED_CURR)
                    write_int(N_WIRED_CURR, DEF_WIRED_CURR);
            } else {
                // 开启时立刻跑一次判定：让"可信性检查"的诊断日志当场落地，
                // 否则用户只看到"已开启"，而真正原因要等 daemon 下一轮才写日志，
                // 容易误以为功能正常（离线测试已复现该困惑）。
                wall_tick();
            }
        }
        int wall_curr = 0;
        read_int(N_WIRED_CURR, &wall_curr);

        // 电池温度必须取"可信源"：优先 profile 里实测确认的独立 NTC，
        // 绝不回退到 battery/batt_temp（它们被 fake_temp 改写，读到的是设定值）。
        int bt = 0;
        long bz = -1;
        int trusted = -1;
        if (load_profile()) {
            trusted = g_prof.batt_trusted ? 1 : 0;
            if (trusted) bz = g_prof.batt_zone;
        }
        if (bz >= 0) {
            // 必须走 wall_real_batt_temp()：内部顺序是
            //   ① 持久 fd（仅 daemon 内有效）→ ② snapshot_fresh() 三重判据 → ③ 返回 0
            // 原来这里 read_live_real 失败后直接 read_int(sysfs)，
            // 而该节点已被 bind mount 伪装 → 读到 350（35.0℃）→ WebUI 温度墙显示假值。
            // 与 read_best() 的修复同源，此处原先漏改。
            bt = wall_real_batt_temp();
        }
        printf("{\"ok\":true,\"wall_on\":%d,\"wall_c\":%d,\"wall_force\":%d,"
               "\"batt_temp\":%d,\"batt_src\":\"zone%ld\",\"batt_trusted\":%d,\"curr\":%d}\n",
               we, wc, wf, bt, bz, trusted, wall_curr);
        return 0;
    }
    else if (!strcmp(cmd, "log")) {
        // 输出日志尾部
        int nl = argc > 2 ? atoi(argv[2]) : 50;
        FILE *fp = fopen(LOG_FILE, "r");
        if (!fp) { printf("(无日志)\n"); return 0; }
        char lines[256][256];
        int cnt = 0, idx = 0;
        while (fgets(lines[idx], 256, fp)) { idx = (idx + 1) % 256; if (cnt < 256) cnt++; }
        fclose(fp);
        int start = (cnt < 256) ? 0 : idx;
        int total = (cnt < 256) ? cnt : 256;
        int from = total > nl ? total - nl : 0;
        for (int i = from; i < total; i++) {
            fputs(lines[(start + i) % 256], stdout);
        }
    }
    else {
        fprintf(stderr, "用法: %s {daemon|apply [温度x10]|restore|status|dump|bright [1-100|auto on|off]|wall [30-60|on|off|trust [on|off]]|log [行数]}\n", argv[0]);
        return 1;
    }
    return 0;
}
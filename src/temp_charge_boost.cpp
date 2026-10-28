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
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctime>

// ============ 配置 ============
static const char *WORK_DIR     = "/data/adb/temp_charge_boost";
static const char *FAKE_DIR     = "/data/adb/temp_charge_boost/fake";
static const char *REAL_DIR     = "/data/adb/temp_charge_boost/real";
static const char *LOG_FILE     = "/data/adb/temp_charge_boost/run.log";
static const char *ZONE_ROOT    = "/sys/devices/virtual/thermal";
static const char *ZONE_CLASS   = "/sys/class/thermal";

static const char *N_FAKE_TEMP  = "/sys/class/xm_power/fuelgauge/strategy_fg/fake_temp";
static const char *N_CTRL_LIMIT = "/sys/class/xm_power/charger/charger_thermal/wired_ctrl_limit";
static const char *N_PPS_PTF    = "/sys/class/xm_power/charger/quick_charge/fake_pps_ptf";
static const char *N_BATT_TEMP  = "/sys/class/power_supply/battery/temp";
static const char *N_USB_ONLINE = "/sys/class/power_supply/usb/online";
static const char *N_BATT_STAT  = "/sys/class/power_supply/battery/status";

static const int  DEF_TEMP      = 350;   // 35.0C
static const int  POLL_CHARGING = 5;     // 充电时轮询秒
static const int  POLL_IDLE     = 30;    // 空闲轮询秒

// 排除的 zone 类型（基带中 -273 的无效传感器）
static const char *EXCLUDE_TYPE[] = { "sdr0_pa", nullptr };

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
    int fd = open(path, O_WRONLY | O_CLOEXEC);
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
    return false;
}

// 判断该 zone 是否需要伪装（排除无效传感器）
static bool zone_wanted(const std::string &type) {
    if (excluded_type(type)) return false;
    // cpu-hw-trip 是硬件跳闸阈值，必须保留真实值
    if (type.find("cpu-hw-trip") != std::string::npos) return false;
    return true;
}

static void ensure_dir(const char *p) {
    mkdir(p, 0755);
}

// ============ 温度伪装 ============

// 前向声明（定义见下方持久 fd 区）
static void open_real_fds();
static void close_real_fds();
static void flush_real_values();

static int apply_temp(int want) {
    ensure_dir(WORK_DIR);
    ensure_dir(FAKE_DIR);
    ensure_dir(REAL_DIR);

    // 关键：必须在挂载前打开持久 fd，才能绕过 bind mount 读到真实值
    open_real_fds();

    // 确保 fake_temp 也设上（官方接口，联动 power_supply）
    write_int(N_FAKE_TEMP, want);

    int ok = 0, skip = 0;
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
        if (fd < 0) continue;
        char buf[16];
        int n = snprintf(buf, sizeof(buf), "%d\n", want);
        ssize_t w = write(fd, buf, n); (void)w;
        close(fd);

        std::string tgt = base + "/temp";
        // 必须先把该目标剥到 0 层再挂；否则 sysfs 上重复 bind 会翻倍膨胀
        if (unmount_all(tgt.c_str()) < 0) { skip++; continue; }
        if (mount(fake.c_str(), tgt.c_str(), nullptr, MS_BIND, nullptr) == 0)
            ok++;
    }
    closedir(d);

    // 充电提速节点
    write_int(N_CTRL_LIMIT, 0);
    write_int(N_PPS_PTF, 1);

    // 挂载完成后写入真实值快照（供 WebUI 跨进程读取）
    flush_real_values();

    fprintf(stderr, "[temp] mounted=%d skipped=%d\n", ok, skip);
    logf("应用温度伪装: 挂载 %d 个节点, 跳过 %d 个, 目标 %d", ok, skip, want);
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
static int  g_bl_max = 0;

static const char *bl_dir() {
    if (g_bl_dir[0] && g_bl_max > 0) return g_bl_dir;
    DIR *d = opendir("/sys/class/backlight");
    if (!d) return NULL;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char dir[320], br[352], mx[352];
        snprintf(dir, sizeof(dir), "/sys/class/backlight/%s", e->d_name);
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
    long now = (long)time(nullptr);
    if (g_auto_cache >= 0 && now - g_auto_ts < 30) return g_auto_cache;
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
    g_auto_ts    = (long)time(nullptr);
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
    g_auto_ts    = (long)time(nullptr);
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
    long last_bl = 0;
    // 开机后立刻把用户设定的亮度落一次（settings 可能被系统重置）
    bl_tick();
    last_bl = (long)time(nullptr);
    for (;;) {
        bool chg = is_charging();
        if (chg != last_chg) {
            logf("充电状态变化: %s", chg ? "开始充电" : "停止充电");
            last_chg = chg;
        }
        if (chg) {
            charge_tick();
            flush_real_values();   // 充电时每次都刷新真实温度
            sleep(POLL_CHARGING);
        } else {
            // 空闲时也定期刷新（每 30 秒）
            flush_real_values();
            sleep(POLL_IDLE);
        }
        // 亮度纠正：每 60 秒最多一次（与充电节奏解耦，避免频繁 fork settings）
        long now = (long)time(nullptr);
        if (now - last_bl >= 60) {
            bl_tick();
            last_bl = now;
        }
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
static std::map<long, int> g_real_fd;   // zone 编号 -> fd

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

// 把所有关键 zone 的实时真实值写入 REAL_DIR（供 WebUI 跨进程读取）
static void flush_real_values() {
    static const long KEY_ZONES[] = { 0, 36, 68, 70, 84 };
    for (long zn : KEY_ZONES) {
        int v = read_live_real(zn);
        if (v <= 0) continue;
        char p[256];
        snprintf(p, sizeof(p), "%s/thermal_zone%ld", REAL_DIR, zn);
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) continue;
        char b[16];
        int n = snprintf(b, sizeof(b), "%d\n", v);
        ssize_t w = write(fd, b, n); (void)w;
        close(fd);
    }
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

    // 真实温度：读电荷泵自带 NTC（cp_master/batt_temp，未被 mount 影响，单位 0.01℃）
    int real_bt = 0;
    {
        read_int("/sys/class/xm_power/cp_master/batt_temp", &real_bt);
        if (real_bt <= 0) {
            // 回退：PMIC 温度
            char p[256];
            snprintf(p, sizeof(p), "%s/thermal_zone86/temp", ZONE_ROOT);
            read_int(p, &real_bt);
        }
    }

    // === 关键指标：真实 vs 伪装（真实值由 daemon 写入 REAL_DIR）===
    int cpu_real      = read_real("thermal_zone0");    // cpullc-0-0
    int cpu_therm_real= read_real("thermal_zone70");   // cpu_therm
    int gpu_real      = read_real("thermal_zone36");   // gpuss-0
    int gpu_kgsl      = 0;
    read_int("/sys/class/kgsl/kgsl-3d0/temp", &gpu_kgsl);
    int batt_real     = read_real("thermal_zone84");   // batt_therm
    int chg_real      = read_real("thermal_zone68");   // charger_therm

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
    bool daemon_alive = false;
    {
        char p[128];
        snprintf(p, sizeof(p), "%s/daemon.pid", WORK_DIR);
        int pid = 0;
        if (read_int(p, &pid) && pid > 0) {
            char sp[64];
            snprintf(sp, sizeof(sp), "/proc/%d", pid);
            daemon_alive = (access(sp, F_OK) == 0);
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
    // 真实 vs 伪装对比（单位 0.001℃，除以 1000 得 ℃）
    printf("\"cpu_real\":%d,", cpu_real);
    printf("\"cpu_therm_real\":%d,", cpu_therm_real);
    printf("\"gpu_real\":%d,", gpu_real);
    printf("\"gpu_kgsl\":%d,", gpu_kgsl);
    printf("\"batt_real\":%d,", batt_real);
    printf("\"chg_real\":%d,", chg_real);
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
    const char *cmd = argc > 1 ? argv[1] : "status";

    if (!strcmp(cmd, "daemon"))       run_daemon();
    else if (!strcmp(cmd, "apply"))   apply_temp(argc > 2 ? atoi(argv[2]) : DEF_TEMP);
    else if (!strcmp(cmd, "restore")) restore_temp();
    else if (!strcmp(cmd, "status"))  show_status();
    else if (!strcmp(cmd, "dump"))    dump_json();
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
        fprintf(stderr, "用法: %s {daemon|apply [温度x10]|restore|status|dump|bright [1-100|auto on|off]|log [行数]}\n", argv[0]);
        return 1;
    }
    return 0;
}
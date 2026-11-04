# 温度伪装 · 充电提速

KernelSU / Magisk 模块。通过 bind mount 伪装系统温度节点，解除充电温控限流，附带充电温度墙与屏幕亮度调节。

作者：雨色

## 功能

### 温度伪装

对 `/sys/devices/virtual/thermal/thermal_zone*/temp` 做 bind mount，覆盖为指定值（默认 35.0℃）。

覆盖 CPU、GPU、电池、充电、PMIC、相机、屏幕、WiFi、DDR 等共 86 个节点。

排除项：
- `cpu-hw-trip-0/1` — 硬件跳闸阈值，保留真实 105℃ 作为最终保护
- `mdmss-0~3`、`pa_therm1/2`、`sdr0`、`mmw_ific0`、`sdr0_pa` — 基带与射频。这些节点真实温度可达 83~89℃，伪装后系统无法对基带降频，故一律保留真实值

温度节点在每次启动时动态扫描 `thermal_zone*/type` 判定，不做 zone 编号硬编码。

### 充电温度墙

按电池真实温度限制充电电流，避免快充过热。阈值默认 51℃，阈值以下完全不干预。

| 电池温度 | 充电电流上限 |
|---|---|
| < 阈值 | 不干预（系统决定） |
| 阈值 ~ +2℃ | 满额的 80% |
| +2 ~ +4℃ | 满额的 50% |
| +4 ~ +6℃ | 满额的 25% |
| > +6℃ | 满额的 12% |

温度回落后自动恢复满额，无需手动干预。

电流上限写的是 `wired_chg_curr`（mi_thermald 的 `thermal_fcc_override` 目标节点），由系统作为充电上限采纳。

**温度来源说明**：`battery` 与 `batt_temp` 两个节点的驱动值会被 `fake_temp` 直接改写，伪装后恒定返回 35℃，无法反映真实温升。因此温度墙与界面显示均取 `batt_therm`（独立 NTC），并通过挂载前保留的 fd 读取，绕过自身伪装。

### 充电提速

| 节点 | 值 | 作用 |
|---|---|---|
| `wired_ctrl_limit` | 0 | 解除限流档位 |
| `fake_pps_ptf` | 1 | 启用 PPS 高压档 |
| `fake_temp` / `battery/temp` | 350 | 官方温度接口 |

### 屏幕亮度

系统把 `settings screen_brightness` 换算后上限压在约 11%，通过直接写 `/sys/class/backlight/*/brightness`（0~16383）绕过该换算。

面板路径自动扫描，找不到可写节点则隐藏该功能，不影响其他部分。

亮度只在你主动操作时改变，守护进程不参与，避免与手动调节冲突。

## 充电节点语义探测

v1.4.0 起，8 个充电/状态节点不再依赖写死的绝对路径，改为**三级解析 + 语义打分**，已有路径只作为参考候选。
换 ROM、换机型、厂商改名、目录结构调整时自动迁移，不需要改代码。

### 解析顺序（命中即止）

| 级 | 来源 | 说明 |
|---|---|---|
| L0 | 环境变量 `TCB_*` | 手动指定优先（调试/特殊机型） |
| L1 | 已知完整路径 | 原硬编码路径，**只作为参考候选**，不存在则自动降级 |
| L2 | 语义探测 | 枚举 `/sys/class/{power_supply,xm_power,thermal}` 两级深度，打分选最优 |

### 打分规则

匹配层级（五档，严格降序，不会出现排序反转）：

| 层级 | 条件 | 基础分 |
|---|---|---|
| 精确 | 文件名 == 目标名 | +100 |
| 别名 | 命中别名表（不同 OS/厂商等价命名） | +85 |
| 前缀 | 以关键词开头 | +75 |
| 包含 | 含关键词 | +65 |
| 后缀 | 以关键词结尾 | +55 |

目录亲和（叠加）：

| 条件 | 分值 |
|---|---|
| 命中该节点声明的 `hint` 目录 | +60 |
| 落在 `/xm_power`、`/power_supply`、`/charger` 体系内 | +20 |
| 两者都不满足 | −30 |

其它修正：值可读 +5、可写 +10。

### 安全阀

模糊匹配（别名/前缀/包含/后缀）必须满足「命中 hint」或「在充电体系内」之一，否则直接淘汰。
实测可挡住 `sys/class/misc/randomdir`、`sys/block/sda` 这类无关目录，不会把充电值写到无关节点上。

`battery/temp`、`usb/online`、`battery/status` 三个 AOSP 标准属性名**不启用模糊匹配**，只认精确名（`temp` 太泛，模糊会误伤）。

### 别名表

| 目标节点 | 接受的等价命名 |
|---|---|
| `wired_chg_curr` | wired_chg_curr / wired_charge_current / chg_curr_limit / charging_current_limit / fast_chg_current / batt_chg_curr |
| `wired_thermal_remove` | wired_thermal_remove / thermal_remove / wired_temp_limit_remove / chg_thermal_remove |
| `wired_ctrl_limit` | wired_ctrl_limit / wired_ctrl / chg_ctrl_limit / charger_ctrl_limit |
| `fake_pps_ptf` | fake_pps_ptf / pps_ptf / fake_pps_passthrough / pps_passthrough |
| `fake_temp` | fake_temp / fake_temperature / emul_temp / sim_temp / simulate_temp |

### 诊断

`scan` 会把每个节点的解析结果写进日志（`run.log`），格式为：

```
[node] 充电电流上限：已知路径 /sys/.../wired_chg_curr 不存在，进入语义探测
[node] 充电电流上限：语义探测命中 /sys/.../chgx/wired_chg_curr（精确匹配，得分 135，枚举 14 个）
[node] 伪温度注入点：语义探测未命中（枚举 9 个文件，目标名 fake_temp）
```

命中会给出匹配层级、得分、枚举文件数；未命中会给出枚举数量与目标名，便于判断是「环境没有」还是「名字不认识」。

### 开销

- 探测只在 `scan` / `apply` / daemon 启动时执行一次，结果缓存在进程内，运行期零开销
- 枚举上限 4000 个文件，单次 `scan` 实测 0.6 秒内完成
- 全路径命中时（已支持机型）零探测、零额外枚举，行为与 v1.3.3 完全一致

## 结构

```
temp_charge_boost/
├── bin/temp_charge_boost     主程序（C++ 静态链接）
├── module.prop               模块信息
├── post-fs-data.sh           开机早期应用伪装
├── service.sh                late_start 启动守护进程
├── uninstall.sh              卸载时完整恢复
├── webroot/index.html        WebUI
└── src/temp_charge_boost.cpp 源码
```

## 命令

```bash
temp_charge_boost daemon              # 常驻守护
temp_charge_boost apply [温度x10]     # 应用一次（默认 350 = 35.0℃）
temp_charge_boost restore             # 恢复全部
temp_charge_boost status              # 文本状态
temp_charge_boost dump                # JSON 状态
temp_charge_boost wall                # 温度墙状态
temp_charge_boost wall 30-60          # 设阈值（℃）
temp_charge_boost wall on|off         # 开关温度墙
temp_charge_boost bright [1-100]      # 屏幕亮度百分比
temp_charge_boost bright auto on|off  # 自动亮度
temp_charge_boost log [行数]           # 日志尾部
```

## WebUI

在 KernelSU / Magisk 管理器模块页面打开。

- **充电** 页：实时功率、伪装状态、真实/伪装温度对比、CPU 各核温度（8 核独立显示，按温度着色）、充电温度墙（开关 + 阈值滑条）、电池信息、运行日志
- **屏幕** 页：亮度滑条、档位预设、自动亮度开关

## 开销

| 指标 | 实测 |
|---|---|
| 内存（VmRSS） | 100~216 KB |
| 线程数 | 1 |
| 文件描述符 | 约 90（86 个温度节点 + 若干） |
| 空闲 CPU | 0.0000% |
| 充电时 CPU | 0.0084% |
| `dump` 单次耗时 | 14~49 ms |

温度伪装为 bind mount，建立后运行期不参与读取；守护进程仅在充电时每 5 秒、空闲时每 30 秒检查一次。

## 兼容性

- 需要 KernelSU 或 Magisk
- 需要 Android 内核支持 bind mount 与 `/sys/class/thermal` 框架
- 充电提速与温度墙不再绑定 `xm_power` 固定路径：优先用已知路径，不存在则语义探测
  `/sys/class/{power_supply,xm_power,thermal}` 下语义等价的节点（支持别名与模糊匹配）
- 完全找不到任何可用的充电节点时，该功能自动停用，不影响温度伪装
- 屏幕亮度依赖 `/sys/class/backlight`，缺失时自动隐藏该功能

主要测试机型：红米 K100 Pro Max（M098FE / songyuan / 内核 6.12.69 / HyperOS 3）

## 注意
温度伪装会让系统读不到真实温度，伪装期间系统温控不会正常介入。基带与 `cpu-hw-trip` 已保留真实值，但电池、电荷泵等仍在伪装范围内，散热与安全由使用者自行负责。

如需恢复系统温控，执行 `temp_charge_boost restore` 并停止守护进程，或直接卸载模块。

## 安装流程

刷入 zip 后按音量键选择，**15 秒不按自动继续**（不会卡住安装）：

| 步骤 | 音量+ | 音量- | 超时 |
|---|---|---|---|
| 选择安装模式 | 标准安装（温度墙 51℃ 开启） | 轻量安装（只伪装温度，温度墙关闭） | 标准安装 |
| 确认安装 | 确认安装 | 取消安装（不做任何改动） | 确认安装 |

安装过程会做三道门禁检查，不满足直接中止并提示原因：

1. 架构必须是 arm64（二进制为 arm64-v8a）
2. 必须存在 `/sys/devices/virtual/thermal`（温度伪装的对象，无替代）
3. 安装包内二进制必须存在

充电节点（`xm_power` 那套）**不参与门禁**：它们由主程序的语义探测解析，
探测不到就自动降级为「仅温度伪装」，不会因此中断安装。

随后自动扫描本机温度节点，按实测语义判定电池真实温度源，结果写入 `/data/adb/temp_charge_boost/profile.conf`，安装日志同目录下 `install.log`（出问题直接贴这个文件）。

## 卸载

管理器内移除模块即可。`uninstall.sh` 会依次：

1. 安全校验模块路径（防误删）
2. 停止守护进程（按 cmdline 精确匹配）
3. **恢复充电电流为满额 22000000** 并把 `wired_ctrl_limit` 归 0
4. 剥离全部温度节点挂载（多层 + 懒卸载兜底）
5. `fake_temp` 归 -999（未启用）
6. 清理 PID / 配置 / 快照 / 日志
7. 标记 `module.prop` 为已卸载

> 第 3 步是关键：若卸载时温度墙正在限流，不恢复电流会让充电永久卡在最低 3A，用户很难自查。

## 版本历史
### v1.4.1 (2026-11-04)
**修复安装脚本的硬编码残留**。v1.4.0 只把主程序的充电节点改成了语义探测，
但 `customize.sh` 的门禁 2 与 `uninstall.sh` 的还原逻辑仍各自写死着绝对路径，
导致换 ROM / 节点改名时**安装直接被 abort**，主程序的探测能力根本来不及生效。

- 🐛 **`customize.sh` 门禁 2 不再硬编码**：删除 `/sys/class/xm_power/fuelgauge/strategy_fg/fake_temp`
  存在性检查。原来缺这一个节点就 `abort`，等于让语义探测形同虚设
- 🐛 **降级而不是阻断**：现在只有「无 thermal 子系统」才 abort（功能根基，无替代）；
  充电体系（`/sys/class/xm_power` 或 `/sys/class/power_supply`）缺失时只提示，
  自动降级为「仅温度伪装」，安装照常完成
- 🐛 **`uninstall.sh` 改为读语义解析结果**：三个充电节点 + `fake_temp` 不再写死，
  优先从 `profile.conf` 的 `node_*` 行取主程序探测到的真实路径；
  profile 缺失时回退已知路径，再不命中则按名字在充电体系内枚举兜底
- ✨ **`profile.conf` 新增 8 条 `node_*` 记录**：主程序把语义探测后的最终落点落盘，
  shell 脚本直接读取，不再维护第二套路径知识
- 🔧 `abort` 调用点从 5 处减到 4 处（架构 / 无 thermal / 缺二进制 / 用户取消）

**验证（离线，不碰真机）**

| 项 | 结果 |
|---|---|
| `customize.sh` 全文无 `fake_temp` 硬编码 | ✅ |
| 仍保留 thermal 子系统硬门禁 | ✅ |
| 充电体系缺失时仅提示、不 abort | ✅ |
| `uninstall.sh` 四个节点均从 profile 读取 | ✅ |
| 改名场景（`charger_thermal` → `chgx`）卸载还原 | ✅ 四值全部写对 |
| profile 缺失时 `pf_get` 返回空、不误写 | ✅ |
| 两个脚本 `sh -n` 语法检查 | ✅ |

### v1.4.0 (2026-11-04)
充电节点从**硬编码**改为**语义探测**。原有 8 个节点的绝对路径降级为「参考候选」，
路径不存在时自动在 `/sys/class/{power_supply,xm_power,thermal}` 内按语义打分寻找等价节点。
换 ROM / 换机型 / 厂商改名不再需要改代码。

- ✨ **三级解析**：环境变量 `TCB_*` → 已知完整路径（参考候选）→ 语义探测
- ✨ **五档匹配打分**：精确 +100 / 别名 +85 / 前缀 +75 / 包含 +65 / 后缀 +55，严格降序
- ✨ **目录亲和**：命中 `hint` 目录 +60 / 充电体系内 +20 / 都不中 −30
- ✨ **别名表**：覆盖 5 个节点的常见等价命名（`chg_curr_limit`、`thermal_remove`、`wired_ctrl`、`pps_ptf`、`emul_temp` 等）
- ✨ **安全阀**：模糊匹配必须命中 `hint` 或在 `/xm_power`、`/power_supply`、`/charger` 体系内，否则淘汰；
  实测可挡住 `sys/class/misc/*`、`sys/block/*` 等无关目录
- 🔧 **`battery/temp`、`usb/online`、`battery/status` 不启用模糊匹配**：AOSP 标准属性名只认精确名（`temp` 太泛会误伤）
- 🔧 **修正排序反转缺陷**：首版前缀分（55）低于包含分（70），会导致 `chg_curr_limit` 输给 `my_chg_curr_xyz`；
  已改为前缀 75 > 包含 65 > 后缀 55
- 🔧 **枚举上限 4000 文件**，单次 `scan` 实测 0.6 秒内完成；探测结果进程内缓存，运行期零开销
- 🔧 **诊断增强**：`scan` 日志给出每个节点的匹配层级、得分、枚举文件数；未命中给出枚举数量与目标名

**离线 mock 回归（7 个场景全过）**

| 场景 | 验证点 | 结果 |
|---|---|---|
| T1 | 五级共存排序（精确 > 别名 > 前缀 > 包含 > 后缀） | ✅ 别名 160 胜出 |
| T2 | 纯模糊层（前缀 150 > 包含 140 > 后缀） | ✅ |
| T3 | `uevent` / `modalias` / `of_node` 排除词过滤 | ✅ 正确跳过 |
| T4 | 只读节点降分 | ✅（proot root 特性，非缺陷） |
| T5 | 改名后 `apply` 写入落到探测节点 | ✅ 4/4 节点写入正确 |
| T6 | 原状路径 0 探测（回归） | ✅ 行为与 v1.3.3 一致 |
| T7 | 目录全删不崩溃 | ✅ rc=0，逐节点报告未命中 |

> 已在支持机型（K100 Pro Max / OS4）上，所有已知路径命中时**探测次数为 0**，零额外枚举，与 v1.3.3 行为完全一致。

### v1.3.3 (2026-10-30)

OS4（OS4.0.5.0.XGNCNXM / Android 17）充电限流适配。**全部基于真机实测，含 mock 回归**。

OS4 把充电限流拆成了三层，模块原先只碰最外层那一个节点，实测提速效果不足：

| 层 | 节点 | 性质 |
|---|---|---|
| 硬件层 | `charger_thermal/wired_chg_curr` | 可写，寄存器上限 15.6A |
| **IC 层** | **`charger_thermal/wired_thermal_remove`** | **可写，OS4 新增，本次接入** |
| 内核层 | `charge_interface/ichg_limit` | 只读动态状态报告，bind mount 无效（已证伪） |

- ✨ **新增 `wired_thermal_remove` 支持**：写 1 解除 IC 级温度限流，实测电流 5.33A → 6.07A（+0.74A）。
  这是实测最有效的单点手段，且不需要动 `wired_chg_curr` 档位。
- 🔧 **`DEF_WIRED_CURR` 从 22A 改为 18A**：OS4 实测 18A 是饱和点，20A/22A 反而回落（寄存器允许 ≠ 有效上限）。
- 🔧 **温度墙与 IC 限流联动**：档位 0（不限流）写 `rm=1`；档位 >0 写 `rm=0`。
  否则软件限流会被硬件绕过——这是逻辑正确性问题，不是优化项。
- 🔧 **`charge_tick` 自愈**：充电 IC 固件或 mi_thermald 会重置该节点，每轮检查并恢复（幂等写，值已正确时跳过，不刷 sysfs）。
- 🔧 **`scan` 新增 `thermal_ok` 探测**，写入 `profile.conf`；OS3 无此节点 → `node_writable` 返回 false → 自动跳过，零副作用。

**mock 回归（4 个场景全过）**

| 场景 | 输入 | 期望 | 实测 |
|---|---|---|---|
| scan 自动适配 | mock 6 zone | `thermal_ok=1`，跳过 mdmss/pa_therm | ✅ 4 可伪装 / 2 跳过 |
| apply 写入 | rm 初值 0 | rm=1 | ✅ 0 → 1 |
| 高温降档 | zone2=56000（56℃），阈值 51℃ | rm=0，curr=3A | ✅ rm=0，18000000→3000000（第 7 档） |
| 低温恢复 | zone2=38000（38℃） | rm=1，curr=18A | ✅ rm=1，3000000→18000000 |

编译：`g++ -std=c++17 -O2 -s -static` → 931392 B（含 64KB 段对齐填充），`-Wall` 零新增告警。

> 注：`g_wall_tier` / `g_wall_push_at` / `g_wall_relax_at` / `g_wall_last` 四个变量的声明
> 从温度墙区上移到 `charge_tick()` 之前——`charge_tick` 需要读 `g_wall_tier` 判断是否
> 维持 `rm=1`，late 声明会直接编译失败。


#### 真机部署后发现并修复的两个缺陷（同日 15:12 验证）

**① 温度墙放档死区（严重，v1.3.2 即存在）**

放档条件写的是 `bt <= relax_c`，而 `relax_c` 固定 `WALL_RELAX_C = 40000`（40℃）。
正常充电时电池温度就是 40~45℃，于是 `40℃ ~ 阈值` 之间形成 11℃ 死区：
**一旦升过档就再也放不回**，除非电池真降到 40℃ 以下。

真机复现（2026-10-30 14:54）：阈值临时设 30℃ → 升到第 7 档、`curr=3000000`；
改回 51℃ 后电池 42.6℃ 落在死区 → 档位永久停在 7 档、电流锁死 3A、
`wired_thermal_remove` 被压成 0（IC 级限流也一并恢复）。

改为 `need == 0`（温度低于阈值即允许计时放档），保留 `WALL_RELAX_SEC = 10` 防抖。
`relax_c` 降级为纯诊断值（`(void)relax_c`）。

**② `wall` 命令与 daemon 抢写 sysfs**

`wall <阈值>` 是独立短命进程，`g_wall_tier` 初值恒为 0，与 daemon 内存态无关。
它调 `wall_tick()` 会用「档位 0」的假状态覆盖 sysfs——daemon 正限流时被强写 `rm=1`，
下一轮 daemon 又写回 0，形成每秒拉锯（真机日志实测 `解除(1)` / `恢复(0)` 相邻两行）。

改为：检测到 daemon 存活（`daemon_running()`，判据同 `status`：pid 存活且
`/proc/<pid>/cmdline` 同时含二进制名与 `daemon`）时只落盘配置并记日志
「温度墙配置已更新，由 daemon 在下一轮生效」；无 daemon 时才允许单次判定。

**验证结果（真机 + mock 双路）**

| 场景 | 修复前 | 修复后 |
|---|---|---|
| 30℃ 升档后改回 51℃，温度不变 42.7℃ | 永久卡第 7 档、3A、rm=0 | 20s 内自动放档、18A、rm=1 |
| `wall 51` 执行时 daemon 在限流 | sysfs 被抢写、日志拉锯 | 只记配置更新，sysfs 不动 |

日志证据（真机 2026-10-30 15:12）：

```
15:12:14 温度墙: 开启, 阈值 30℃
15:12:14 温度墙配置已更新，由 daemon 在下一轮生效
15:12:16 充电温度限流: 恢复 (wired_thermal_remove=0)
15:12:16 温度墙: 电池 42.8℃ (阈值 30.0℃)，充电电流上限 18000000 -> 3000000 (8%，第 7 档)
15:12:22 温度墙: 开启, 阈值 51℃
15:12:22 温度墙配置已更新，由 daemon 在下一轮生效
15:12:36 电池 42.7℃ 已回落，恢复充电电流 18000000
15:12:36 充电温度限流: 解除 (wired_thermal_remove=1)
```

编译：`g++ -std=c++17 -O2 -s -static` → 931392 B（`.tdata` 起段文件偏移后移 0x10000，
`.text` 仅 +4336 B，属 section-to-segment 对齐填充；虚拟地址与 `.data`/`.bss` 完全一致）。

### v1.3.2 (2026-10-29)
补 v1.3.1 漏改的同类缺陷——**部署到真机后实测发现，不靠推断**。

v1.3.1 把「时间基准」改成了单调时钟，但只改了一部分，三处仍用墙钟，其中一处在安全关键路径上：

- 🐛 **`wall_real_batt_temp()` 仍用 `time(nullptr) - st_mtime` 判快照新鲜度**（温度墙实际使用的取温函数）。
  时间跳变时 age 变负 → 快照丢弃 → 返回 0 → **温度墙静默停用**。改为复用 `snapshot_fresh()` 三重判据。
  这是线上「真实温度 0.3℃」事故的**真正落点**：daemon 那条路径修了，温度墙这条没修。
- 🐛 **`wall` 命令回退直读 sysfs**：`read_live_real` 失败后直接 `read_int(thermal_zoneN/temp)`，
  而该节点已被 bind mount 伪装 → 返回 350（35.0℃）。改为复用 `wall_real_batt_temp()`，与 daemon 同判据。
- 🐛 **亮度缓存时钟混用**：`bl_auto_mode_cached()` 用 `mono_ms()` 写缓存，但 `bl_set_manual`/`bl_set_auto`
  却写 `(long)time(nullptr)` → 两个时钟混用，缓存判据永远错（要么永久命中、要么永久失效）。

**真机验证（本机 Redmi K100 Pro Max / M108FE / OS3.0.310 / Android 16）**

| 验证项 | 结果 |
|---|---|
| 节点语义实测 | zone84=`batt_therm` 独立 NTC（Δ=-32 不跟随），`batt_trusted=1`，zone67/68 归兜底 |
| 快照 mtime 改成 1970 + 停 daemon | `wall` 仍读 `batt_temp=51621`（真实值），未回退到伪装值 350 |
| PID 复用（旧 pid 指向 com.miui.daemon） | `daemon_alive=false`，不再误报 |
| 温度墙按真实温度工作 | 50.9℃ 起逐档 60%→48%→36%→26%→16%→8%，降到 51.2℃ 后自动回升至 72% |

> 注：`real_batt_temp` 取 `cp_master/batt_temp`（电荷泵自测电池温度，单位 0.01℃），
> 与独立 NTC 存在温差属正常（实测 64.0℃ vs 51.2℃，因电荷泵自身发热）。
> WebUI 主显示用 `batt_ntc`（独立 NTC），此字段仅作参考。

### v1.3.1 (2026-10-29)
修一批**只在长时间运行 / 异常环境才暴露**的真实缺陷。触发场景：系统时间跳变（RTC 未同步）+ PID 复用。

- 🐛 **时间基准全面换成单调时钟**。原来所有「多久之后做某事 / 多久之前写的」判断都用 `time()`，
  一旦系统时间被跳回过去，差值变负 → 快照永不刷新、温度墙时间积分项失灵、亮度缓存永久命中。
  新增 `mono_ms()`（`CLOCK_MONOTONIC`），统一用于快照刷新、快照新鲜度、温度墙积分、亮度缓存。
- 🐛 **快照新鲜度改为三重判据**：`.meta` 里的 `boot_id` 必须等于当前值 + 写入时刻用单调毫秒 + 年龄 ≤ 120 秒。
  原来只看 `mtime` 差值，时间倒退时得负数 → 快照全被丢弃 → 回退读 sysfs 读到伪装值（表现为「真实温度 0.3℃」）。
- 🐛 **回退读 sysfs 前先检查是否已被本模块伪装**。已被接管则返回 0 而不是假值
  （「0 至少能看出读不到，假值会被当真」）。
- 🐛 **`real_batt_temp` 排除电荷泵陈旧值**：`cp_master/batt_temp` 有效范围 1000~9999 且要求 `ibus > 10`，
  否则回退到独立 NTC `batt_therm`。**禁止回退到 `charger_therm`**——它也在伪装范围内，回退过去等于换个假值（实测显示 35.0℃）。
- 🐛 **`daemon_alive` 不再只看 PID 文件**：改为校验 `/proc/<pid>/cmdline` 同时命中 `temp_charge_boost` 与 `daemon`。
  原来 PID 被内核复用给别的进程（实测被 `com.miui.daemon` 占用）时，WebUI 会显示「守护进程运行中」但实际早已死掉。
  注意这两件事可以同时发生：daemon 真的在跑、而 pid 文件里记的旧 PID 已被复用，此时旧代码的「运行中」是巧合正确。
- ✅ 测试扩到 121 项断言（新增 L 时间跳变 / M 伪装值不得当真值 / N 电荷泵陈旧值 / O PID 复用）
- ✅ 测试基础设施加固：抽出 `mock_env.sh` 统一搭环境，消除 `logic_test` 对 `mock_test` 的隐式顺序依赖；
  每个测试自建环境、自收拾挂载残留；解析不到源码常量直接失败而不是静默降级

### v1.3.0 (2026-10-29)
- ✅ 安装时自动扫描温度节点，按**实测语义**判定电池真实温度源（改 `fake_temp` 观察谁跟随，不靠名字猜）
- ✅ 按系统版本（OS3 / OS4 / 通用）选择候选顺序，换机型靠实测兜底
- ✅ 新增 `customize.sh`：音量键选择安装模式 + 确认安装，超时自动继续
- ✅ 新增 `uninstall.sh`：完整还原（含充电电流、多层解挂载、MODDIR 安全校验）
- ✅ `restore` 命令补上充电电流恢复（原实现只解挂载，会把充电卡在限流值）
- 🐛 语义探测改三态：判定失败不再被当成「可信」，避免拿假值控温
- 🐛 探测失败零污染：统一恢复出口 + 扫描前后双层兜底，`fake_temp` 必定还原
- 🐛 跟随判定阈值单位自适应（兼容 0.1℃ 与毫摄氏度两种节点）
- 🐛 无独立 NTC 时温度墙停用并输出原因（原来静默不工作）
- ✅ 新增 `wall trust` 强制启用逃生门
- ✅ 离线模拟测试 6 套共 111 项断言

### v1.2.0
- ✅ 新增充电温度墙（51℃ 前不限流，降到 40℃ 恢复满额）
- ✅ CPU 各核温度展示
- ✅ 基带节点排除（保留真实值防掉信号）

### v1.1.0
- 温度伪装 + 充电提速 + 屏幕亮度调节

## 许可与免责

### 开源协议
MIT License

### 免责声明
本模块仅供学习研究使用。使用本模块：
- ⚠️ 会改变系统温度读数，系统温控在伪装期间不介入
- ⚠️ 需要 Root 权限，存在系统风险
- ⚠️ 作者不对任何设备损坏、数据丢失等后果负责
- ⚠️ 请在了解原理和风险的前提下使用

### 信息
- **作者**: 雨色
- **架构**: arm64-v8a
- **系统**: Android 16 / HyperOS 3（充电相关功能依赖澎湃 OS）
- **Root 框架**: KernelSU / Magisk / APatch
- **版本**: v1.3.0
- **更新**: 2026-10-29

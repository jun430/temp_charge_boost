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
- 充电提速与温度墙依赖小米 `xm_power` 节点，缺失时自动跳过，不影响温度伪装
- 屏幕亮度依赖 `/sys/class/backlight`，缺失时自动隐藏该功能

主要测试机型：红米 K100 Pro Max（M098FE / songyuan / 内核 6.12.69 / HyperOS 3）

## 注意

温度伪装会让系统读不到真实温度，伪装期间系统温控不会正常介入。基带与 `cpu-hw-trip` 已保留真实值，但电池、电荷泵等仍在伪装范围内，散热与安全由使用者自行负责。

如需恢复系统温控，执行 `temp_charge_boost restore` 并停止守护进程，或直接卸载模块。

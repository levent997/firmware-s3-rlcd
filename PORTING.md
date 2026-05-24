# 移植对比 · 官方 M5StickC Plus 固件 vs 本项目

本文档逐项对比 `claude-desktop-buddy/src/`（官方 M5StickC Plus 参考固件，
下文称 **官方**）与 `firmware-s3-rlcd/src/`（本项目，**本机**），用来：

1. 复核协议覆盖率（哪些字段官方处理了我们没处理）
2. 复核硬件相关功能的差异（哪些受硬件限制做不到，哪些只是没做）
3. 给出**可移植但未做**的功能 TODO，按优先级排序

---

## 1. 文件 / 模块对照

| 关注点 | 官方文件 | 本机文件 | 状态 |
|---|---|---|---|
| BLE NUS 外设 | `ble_bridge.{h,cpp}` (Bluedroid) | `ble_nus.{h,cpp}` (NimBLE) | ✅ 协议 + LE Secure Connections 加密 |
| 协议解析 | `data.h` (含演示模式) | `protocol.{h,cpp}` | ⚠️ 缺 demo 模式 + entry 缓冲量 |
| NVS 持久化 | `stats.h` | `persist.{h,cpp}` | ✅ 已做 |
| 文件夹推送 | `xfer.h` (LittleFS) | — | ❌ **未做** |
| 自定义角色包 | `character.{h,cpp}` (AnimatedGIF) | `sprites.h` (预转换) | ➖ 设计上不同：我们用静态 PROGMEM |
| 角色物种 | `buddy.{h,cpp}` + `buddies/` 18 种 | `sprites.h` 1 套（Clawd 13 动画） | ➖ 设计上不同：我们走 GIF→bitmap，不用 ASCII pet |
| 主循环 / UI | `main.cpp` (1265 行) | `main.cpp` + `ui.cpp` | ⚠️ 缺菜单、Info、Clock 屏 |
| 传感器 | M5 内置 IMU / AXP192 | `sensors.{h,cpp}` SHTC3 + ADC | ➖ 不同硬件，行为不一样 |

---

## 2. 协议字段覆盖

REFERENCE.md 心跳 snapshot 字段：

| 字段 | 官方 | 本机 | 备注 |
|---|---|---|---|
| `total` | ✅ | ✅ | |
| `running` | ✅ | ✅ | |
| `waiting` | ✅ | ✅ | |
| `msg` | ✅ | ✅ | |
| `entries` | ✅ 最多 **8** 行 × 91 字符，可滚动查看 | ✅ 8 行存储；MAIN 显示最近 3 行；长按 KEY/BOOT 唤出 8 行 history overlay | |
| `tokens` | ✅ + NVS delta 累加 | ✅ + NVS 累加（`tokens_boot`）| |
| `tokens_today` | ✅ | ✅ | |
| `prompt` | ✅ approve/deny 全闭环 | ✅ KEY=approve / BOOT=deny + velocity 记录 | |

REFERENCE.md `cmd → ack` 表：

| 命令 | 官方 | 本机 | 备注 |
|---|---|---|---|
| `cmd:status` | ✅ 完整 data 块（name/sec/bat/sys/stats）| ✅ name/sec/sys/stats 真值（bat 见 SYSTEM 视图）| |
| `cmd:name` | ✅ + NVS 写盘 | ✅ + NVS 写盘 | |
| `cmd:owner` | ✅ + NVS 写盘 | ✅ + NVS 写盘 | |
| `cmd:unpair` | ✅ 清 bond | ✅ 清 bond + 擦 NVS | |
| 时间同步 `{"time":[...]}` | ✅ + 写入 RTC | ⚠️ 仅内存计算，没写 PCF85063 RTC | **可补强**（PCF85063 已在板） |

REFERENCE.md 折叠协议（folder push）：

| 步骤 | 官方 | 本机 |
|---|---|---|
| `char_begin` / `file` / `chunk` / `file_end` / `char_end` | ✅ 完整闭环 + base64 解码 + LittleFS 写盘 | ❌ 直接 ack=false 拒收 |

REFERENCE.md turn 事件：

| 字段 | 官方 | 本机 |
|---|---|---|
| `{"evt":"turn",...}` | ❌（官方也没用） | ❌ |

---

## 3. 安全 / 配对

| 关注点 | 官方 | 本机 |
|---|---|---|
| 加密配对 | ✅ `SC_MITM_BOND`，CCCD 设 `PERM_ENCRYPTED` | ✅ NimBLE `setSecurityAuth(true,true,true)` + `READ_ENC`/`WRITE_ENC` |
| 显示 passkey | ✅ 单独 `drawPasskey()` 屏 | ✅ `ui.cpp::drawPasskeyScreen` 50px 大字 |
| Bond 清除（`cmd:unpair`）| ✅ `bleClearBonds()` | ✅ `NimBLEDevice::deleteAllBonds()` |
| Status ack `sec:true` | ✅ 真值 | ✅ `g_state.secure` 真值 |
| 顶栏加密指示 | — | ✅ `BT*` 表示已加密 |

**P0 已补完** — transcript 不再明文飞 2.4GHz。

---

## 4. 数据结构 / 数值模型

### 4.A 数值模型对照

| 字段 | 官方（`src/stats.h`）| 本机（`src/state.h`） |
|---|---|---|
| `tokens` 累加 | ✅ delta 累加，bridge 重启不丢 | ✅ `tokens_boot` 累加 + NVS |
| `level` | ✅ `tokens / 50000` | ✅ `tokens_boot / 50000` (持久) |
| Energy tier | ✅ 0-5，2h/tier 衰减，nap end 满血 | ✅ 同（nap 触发改用 BLE 断开 > 5min）|
| Fed pip | ✅ `(tokens % 50000) / 5000` | ✅ 同 |
| Velocity ring buffer | ✅ 最近 8 次 approval 响应时延，驱动 mood | ✅ 已写入 + SYSTEM 视图直方图 + 联动 mood |
| approval/denial counter | ✅ + NVS | ✅ + NVS |
| `napSeconds` 累计 | ✅ + NVS | ⚠️ 没累加 |
| 设备名 / owner 名 | ✅ + NVS | ✅ + NVS |
| species 索引 | ✅ + NVS | ➖ 不适用（我们没 ASCII pet 概念）|

### 4.B Entry 缓冲

| 维度 | 官方 | 本机 |
|---|---|---|
| 最大条数 | 8 | ✅ 8（存储），MAIN 渲染显示最近 3 |
| 每条长度 | 91 字符 | ~56 字符（W=400 / 7px 字宽，超出 `~` 截断）|
| 历史浏览 | ✅ B 键 / `msgScroll` | ✅ MAIN 视图长按 KEY/BOOT 唤出全屏 history overlay |
| `nLines=0` 回退到 `msg` | ✅ | ✅（最近刚补上）|

### 4.C 默认设置

| 设置 | 官方默认 | 本机 |
|---|---|---|
| sound | ✅ on | ❌（板子带 ES8311 codec，未驱动） |
| bt | ✅ on | ✅ |
| wifi | ✅ stub | ✅ stub |
| led | ✅ on | ❌（板子没专用 LED）|
| hud | ✅ on | ➖ 我们总是显示 HUD |
| clockRot | ✅ 自动/竖/横 | ➖ 我们固定横屏 |

---

## 5. UI 模式 / 屏幕状态机

| 屏幕 | 官方 | 本机 |
|---|---|---|
| Pet（主屏 + 角色 + HUD）| ✅ | ✅ MAIN 视图 |
| Info（多页 stats 含 velocity 直方图）| ✅ 3 页 | ⚠️ 部分内容散在 USAGE / SYSTEM，缺直方图 |
| Clock | ✅ 大数字时钟独立屏 | ❌ 仅顶栏小时钟 |
| Approval（待审批专用屏 + 倒计时）| ✅ | ✅ `drawApprovalView()` 全屏接管 |
| Menu（长按 A）| ✅ | ❌ |
| Settings | ✅ | ❌ |
| Reset / Factory Reset | ✅ | ⚠️ `cmd:unpair` 通过 BLE 触发，无 UI 入口 |
| Passkey | ✅ | ✅ `drawPasskeyScreen()` |
| 演示模式 | ✅ 长按某键进入 | ⚠️ 仅 idle 时自动轮播 sprite |

---

## 6. 硬件交互对照

| 功能 | 官方 (M5StickC Plus) | 本机 (Waveshare ESP32-S3-RLCD-4.2) |
|---|---|---|
| 屏 | TFT 135×240 彩色 | RLCD 300×400 单色（更大、反射）|
| 按键 | A、B、电源键 3 个 | KEY、BOOT 2 个 + 电源 PMIC |
| IMU | ✅ MPU6886（摇晃 → dizzy，翻面 → nap）| ❌ 无 |
| LED | ✅ 红色指示灯 | ❌ |
| 蜂鸣器 | ✅ 内置 piezo | ❌（有 ES8311 + 扬声器但未驱动）|
| 麦克风 | ❌ | ✅ 双麦阵列（未用）|
| 温湿度 | ❌ | ✅ SHTC3 |
| 实时时钟 | ✅ AXP192 内置 | ✅ PCF85063 外接（未驱动写入）|
| SD 卡 | ❌ | ✅（未驱动）|
| 充电检测 | ✅ AXP192 STAT 直读 | ⚠️ 启发式电压趋势 |
| 屏背光 | ✅ AXP192 可调亮度 | ➖ RLCD 是反射屏无背光 |
| 自动息屏 | ✅ 30s 无交互 | ❌ |
| Face-down 翻面睡 | ✅ IMU 检测 | ➖ 用 "BLE 断开 > 5min" 近似 |
| Shake-to-dizzy | ✅ IMU 检测 | ❌ |

---

## 7. 已做对 / 已做好

| 项目 | 备注 |
|---|---|
| 协议核心（heartbeat 解析 + 主要 ack）| 数据字段读取齐全 |
| BLE NUS 广播 + 命名 `Claude-XXXX` | 桌面 picker 能扫到 |
| 5 档 energy + 10 格 fed + level | 对齐官方 `stats.h` 数值模型 |
| 16 个 Clawd 像素动画 | 用 `tools/gif_to_sprites.py` 从官方 GIF 转 |
| Sprite 底部对齐 | 修了 CELEBRATE 错位 |
| 顶栏：名字 / BT / WiFi / 温湿度 / 时钟 / 电量 / 充电闪电 | 信息密度高 |
| 三视图切换（MAIN / USAGE / SYSTEM）| 不同硬件不同需求 |
| 1h / 5h 本地 token 滚动估算 | 协议没真值，显式标注估算 |
| 进度条 + 重置时间布局 | 对齐 claude.ai `/usage` 视觉风格 |
| 走动小 Clawd（SYSTEM 屏底部）| 16s 一轮 |
| Showcase 轮播 16 个 sprite | 80s 完整一轮 |

---

## 8. TODO（可移植但未做）

按价值 × 工作量排序，**P0 最重要**：

### ✅ P0：加密配对（链路安全）— **完成** (commit `dd29e52`)
- NimBLE `setSecurityAuth(bond=true, mitm=true, sc=true)`
- `setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY)` + `onPassKeyRequest` 生成 6 位随机 passkey
- TX/RX 加 `READ_ENC` / `WRITE_ENC` 权限，强制 bond 才能 GATT
- `onAuthenticationComplete` 写 `g_state.secure`，status ack 真值
- 顶栏 BT 指示器变成 `BT*` 表示链路已加密
- `cmd:unpair` 真正调用 `NimBLEDevice::deleteAllBonds()`
- 全屏 passkey 显示页（`ui.cpp::drawPasskeyScreen`），50px 大字

### ✅ P0：NVS 持久化（重启不丢数据）— **完成** (commit `e314a9b`)
- `Preferences` 命名空间 `"buddy"`
- 持久化：`tokens_boot` / `level` / `approvals` / `denies` / `turns_done` /
  `energy_at_nap` / `petName` / `ownerName`
- 写盘时机：approval / denial / nap end / 命令收到时；token progress 节流
  到每 5 分钟或 5K token 增量或 level 跨越
- 模块：`src/persist.{h,cpp}`
- `cmd:unpair` 顺带 `persist::wipe()`

### ✅ P1：Approve / Deny 按键流 — **完成** (本次提交)
- `prompt.active` 时按键语义临时切换：
  - **KEY 短按** = approve (`decision: once`)
  - **BOOT 短按** = deny (`decision: deny`)
  - **任一长按** = 忽略，做视图导航逃生
- MAIN 视图全屏覆盖为 `drawApprovalView()`：notification sprite + 大字
  "APPROVE?" + Tool + 自动换行的 hint + 等待秒数 + 大键位提示
- 8 槽 velocity ring buffer 记录响应时延
- 底栏按键提示语随状态切换

### ✅ P1：Velocity 直方图（小图表）— **完成** (本次提交)
- ring buffer 由 `protocol::sendPermission` 写入（P1-3）
- `ui.cpp::drawVelocityHistogram` 渲染 8 格柱状图
  - 位置：SYSTEM 视图底部条带，**有数据**时替换走动小 Clawd；无数据时保留 mascot
  - 比例尺：0–60s；超出则在柱顶画一个向上的小三角
  - 标签：每根柱上方显数值（秒），左/右轴标 `oldest`/`newest`
  - 表头同行附 `avg / min / max / n=N/8` 一行摘要
- `moodAdjective()` 联动：velocity 平均 `<5s → +1` energy tier、`>30s → -1`
- `hashState()` 把 `velocity_count` 和 `velocity_idx` 计入，新一次审批立刻触发重画

### P1：Folder push（自定义角色包）
- 我们的 sprite 是编译期静态的；要支持运行时自定义需要：
  - LittleFS 分区
  - `cmd:char_begin / file / chunk / file_end / char_end` 接收 + base64 解码
  - 运行时 GIF 解码（`bitbank2/AnimatedGIF`）
  - 退出条件 / 文件验证 / 路径校验（防 `..`）
- 参考代码：`src/xfer.h`（含 9 个 ack 状态机）
- 价值：能在不重新编译固件的情况下换角色

### P2：Time sync 写入 PCF85063
- 现在 `{"time":[epoch, tz]}` 只存内存
- 加 PCF85063 I2C 驱动，写入 RTC 寄存器
- BLE 断开期间时钟不丢，重连前也能在顶栏显示正确时间

### ✅ P2：扩 entries 缓冲到 8 条 + 历史浏览 — **完成** (本次提交)
- `g_state.entries[3]` → `[8]`，`protocol.cpp` 解析 8 条
- `g_state.history_open` 标志
- MAIN 视图长按 KEY 或 BOOT → 唤出 `drawHistoryOverlay()` 全屏层
  - 标题栏 `Transcript history  N/8  msg: ...`
  - 8 行：编号 + 截断的内容（`~`），空槽显 `(empty)`
  - 底部双行：左 `[any key] close history overlay`，右 `live heartbeat / stale`
- 任意按键关闭 history overlay
- 主屏底栏提示语在 MAIN 视图加 `long-press = history`

### P2：演示模式（fake heartbeat）
- 官方 `data.h:130-170` 有 `_FAKES[]` 数组循环假数据
- 不连 BLE 时手动按组合键进入演示，方便测 UI 不需要真 Claude
- 我们的 idle showcase 只切 sprite，没切心跳字段 → 数字静止
- 演示模式让 `tokens` / `running` / `prompt` 也动起来

### P3：菜单系统（长按 KEY 进入）
- 设置项：声音开关 / WiFi 开关 / 时钟模式 / 重置统计 / 工厂复位
- 用按键导航：BOOT 切项，KEY 确认
- 参考代码：`src/main.cpp:322-355 drawMenu()`

### P3：音频反馈
- ES8311 codec 已经在板，I2S 引脚已在硬件
- approval 来时播 ding，error 来时播 buzz
- 涉及 `i2s_driver_install` + 一小段 PCM 数据嵌入 PROGMEM

### P3：演示视频 / Demo 文档
- 录 10 秒视频展示三视图 + 配对流程
- README 顶部加 GIF 或图

### P4（可能不做）

| 项目 | 为什么也许不做 |
|---|---|
| 多 ASCII pet 物种切换 | 我们的设计是固定 Clawd 像素，更"官方风格" |
| Face-down nap / Shake-to-dizzy | 板子没 IMU，要外接，性价比不高 |
| AXP192 屏亮度 | RLCD 反射屏无背光，无意义 |
| Turn event 接收 | 4KB 上限，价值低，复杂度高 |

---

## 9. 验收脚本（同步官方时检查清单）

每次从官方 `src/` pull 新逻辑过来，按这个清单跑一遍：

1. **协议**：心跳所有字段都解析；`cmd:status` 回 `data` 块；`cmd:name/owner/unpair` 都 ack
2. **NVS**：拔电再上电，tokens / level / counters 不归零
3. **加密**：sniffer 抓包，应该看不到明文 JSON
4. **视图**：MAIN / USAGE / SYSTEM 都能切；不会卡在某一视图
5. **Sprite**：13+ 个动画都在 80s carousel 里出现过
6. **温湿度**：SHTC3 读出来的数 vs 室温合理
7. **充电**：插 USB 5 秒内电池图标出现闪电
8. **断连恢复**：拔 BLE 5 分钟后再连，energy 回到 5/5

---

## 10. 一句话总结

| 大类 | 完成度 |
|---|---|
| 核心心跳字段 | 100% |
| 命令 ack（含持久化）| 100% |
| Approval 闭环（按键 + velocity）| 100% |
| BLE 加密配对（LE SC + bond + passkey）| 100% |
| NVS 持久化 | 100% |
| Folder push（自定义角色包）| 0% |
| 菜单 / 设置 / 重置 UI | 0% |
| Clock 独立屏 + RTC 写入 | 0% |
| 演示模式（fake heartbeat）| 部分（仅 sprite 轮播）|

**两条 P0 通路（加密 + NVS）都已打通**，本机现在**功能等价**于官方固件可以日用。
剩余 P1/P2/P3 都是体验/装饰类增量，按需补。

## 11. 进度记录

| 日期 | Commit | 改动 |
|---|---|---|
| 初版 | `e8a7f06` | PORTING.md 落地 |
| P0-1 | `e314a9b` | NVS 持久化 |
| P0-2 | `dd29e52` | LE Secure Connections 加密 + passkey 屏 |
| P1-3 | `73fc03e` | Approval 按键流 + velocity ring buffer + 全屏审批视图 |
| P1-4 | `c7040d7` | Velocity 直方图（SYSTEM 视图） + mood 联动 |
| P2-5 | `b6fbf3a` | entries 缓冲 3→8 + 长按 history overlay |

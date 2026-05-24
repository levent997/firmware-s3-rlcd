# Claude Hardware Buddy · ESP32-S3-RLCD-4.2 固件

把 Waveshare ESP32-S3-RLCD-4.2 开发板变成一个 Claude 桌面 app 的
**Hardware Buddy**：在 4.2 英寸反射屏上画一只像素风 Clawd 桌宠，
显示当前会话状态、token 用量、温湿度、电量等信息，按键切换四个仪表盘视图。

通过蓝牙 LE 的 Nordic UART Service 协议跟 Claude for macOS / Windows
桌面客户端的 **Help → Troubleshooting → Enable Developer Mode →
Developer → Open Hardware Buddy…** 配对。配对走 LE Secure Connections +
6 位 passkey 显示在屏幕上，链路全程加密。

---

## 它能干什么

- **实时状态显示** —— running / waiting / done / approve? / error，桌面端 BLE 心跳每几秒更新一次
- **16 套 Clawd 像素动画**（idle / idle_reading / bubble / building / typing / thinking /
  sweeping / juggling / carrying / headphones / happy / notification / double_jump /
  annoyed / error / sleeping），按桌面状态自动切换
- **4 个仪表盘视图**：MAIN（主面板）/ USAGE（用量）/ SYSTEM（诊断）/ CLOCK（大字时钟）
- **板载传感器全部接通**：SHTC3 温湿度、PCF85063 RTC（时间跨重启不丢）、18650 电池电量 + 充电检测
- **审批闭环**：active prompt 时 KEY 短按 = approve，BOOT 短按 = deny，全屏覆盖、velocity ring buffer 记录响应时延
- **ES8311 音频反馈**：approve → ding / deny → buzz / error 心跳 → buzz / 开机自检 chirp
- **自定义角色包**：从桌面端拖拽推送 `<name>.gif` 到 LittleFS，运行时解码替换 sprite，不用重烧固件
- **历史 transcript overlay**：MAIN 视图长按调出最近 8 条 transcript 全屏视图
- **设置菜单**：USAGE 视图长按 → 6 项设置（声音 / 复位统计 / 删角色包 / 工厂复位 / 重启 / 取消），destructive 项二次确认
- **演示模式**：SYSTEM 视图长按 → 7 场景假心跳轮播，不联 Claude 也能看 UI 动起来
- **NVS 持久化**：tokens / level / approvals / denies / turns / 设备名 / 主人名 / 声音设置 跨重启保留
- **加密配对**：LE Secure Connections + MITM bonding + 屏上显示 6 位 passkey，顶栏 `BT*` 表示链路已加密
- **协议层完整 ack**：`status` / `name` / `owner` / `unpair` / 折叠推送 5 步状态机（`char_begin` / `file` / `chunk` / `file_end` / `char_end`）

---

## 硬件清单

| 模块               | 详情                                          |
|--------------------|-----------------------------------------------|
| 主控               | ESP32-S3-WROOM-1-N16R8 (16MB Flash / 8MB OPI PSRAM) |
| 屏幕               | 4.2" 反射式 LCD 300×400，ST7305 控制器          |
| 蓝牙               | BLE 5.0，Nordic UART Service，LE SC + bond     |
| 温湿度             | SHTC3 @ I2C 0x70                              |
| 实时时钟           | PCF85063 @ I2C 0x51                           |
| 音频               | ES8311 codec @ I2C 0x18 + I2S TX +扬声器        |
| IMU                | QMI8658C @ I2C 0x6A/0x6B（schematic 有，**本机 DNP 未贴片**，驱动已就绪） |
| 电池               | 18650 锂电池座 + ADC1_CH3 分压采样，启发式充电检测 |
| 存储               | 16MB Flash 分区 4MB app + 11.8MB LittleFS；Micro SD 卡槽（未使用） |
| 按键               | KEY (GPIO18) + BOOT (GPIO0) + POWER (PMIC，软件不可读) |

完整引脚表见 [CLAUDE.md](CLAUDE.md)。

---

## 快速上手

### 一、装环境

```bash
pip install -U platformio
```

### 二、连板子

USB-C 接 ESP32-S3 板子。Windows 设备管理器里应该出现 COM 口（一般是
COM3）。Linux/macOS 是 `/dev/ttyACM0` / `/dev/cu.usbmodem*`。

### 三、编译 + 烧录

```bash
pio run --upload-port COM3 -t upload
```

Windows 用户也可以双击仓库根目录的 `flash.bat` 一键搞定。

### 四、看串口

```bash
pio device monitor --port COM3 --baud 115200
```

或者双击 `monitor.bat`。

⚠️ **`monitor` 和 `upload` 不能同时占用同一个 COM 口**，烧之前要先关掉
监控窗口。

### 五、跟 Claude 配对

1. 打开 Claude 桌面 app
2. **Help → Troubleshooting → Enable Developer Mode**
3. **Developer → Open Hardware Buddy…**
4. 点 **Connect**，选 `Claude-XXXX`（XXXX 是设备 MAC 后 4 位）
5. 屏幕上弹出 6 位 passkey，在桌面端输入完成配对
6. 顶栏 `BT` 变成 `BT*` 表示链路已加密

配对成功后桌面 app 自动推送心跳，设备屏幕即时更新。

---

## 按键映射

板子物理上只有 KEY (GPIO18) 和 BOOT (GPIO0) 两个软件可读的按键，
长按 / 短按 + 当前视图组合出全部交互：

| 场景 | KEY 短按 | BOOT 短按 | KEY 长按 | BOOT 长按 |
|---|---|---|---|---|
| **MAIN** 视图 | 下一视图 → USAGE | 上一视图 → CLOCK | 打开 transcript history overlay | 同 KEY 长按 |
| **USAGE** 视图 | 下一视图 → SYSTEM | 上一视图 → MAIN | 打开 **设置菜单** | 同 KEY 长按 |
| **SYSTEM** 视图 | 下一视图 → CLOCK | 上一视图 → USAGE | 切换 **演示模式**（跳到 MAIN） | 同 KEY 长按 |
| **CLOCK** 视图 | 下一视图 → MAIN | 上一视图 → SYSTEM | 下一视图（同短按） | 上一视图 |
| **active prompt** 弹起时 | **APPROVE**（播 ding） | **DENY**（播 buzz） | 打开 history overlay（不影响 prompt 状态） | 同 KEY 长按 |
| **设置菜单** 打开时 | 下一项 | 上一项 | 激活 / 确认 destructive 项 | 返回 / 取消 |
| **菜单 confirm 页** | — | — | 真正执行 destructive 操作 | 取消，回菜单 |
| **history overlay** 打开时 | 关闭 | 关闭 | 关闭 | 关闭 |
| **passkey 显示页** | 不响应 | 不响应 | 不响应 | 不响应 |

---

## 四个视图

按 **KEY** 下一屏，**BOOT** 上一屏。

### 1. MAIN（主视图）

```
┌──────────────────────────────────────────┐
│ Clawd  [DEMO?]    □BT* WiFi 31°C  16:42 │
├──────────────────────────────────────────┤
│              │ Mood    focused           │
│   [SPRITE]   │ Energy  [▮▮▮▮▯]   4/5     │
│   Clawd      │ Fed     [██████▒▒▒▒] 7/10 │
│   像素图     │ Level   96  -> L97 in 6K  │
│  [WORKING]   ├───────────────────────────┤
│              │ Now  2 sessions for 3m12s │
│              │ desk 142K   today 223K    │
│              │ rate  142/min   up  2h05m │
│              │ * output tokens only      │
├──────────────────────────────────────────┤
│ Recent activity      msg: done, 13 turns │
│ > Bash: git status                       │
│ > Read: src/ui.cpp                       │
│ > Edit: src/main.cpp                     │
└──────────────────────────────────────────┘
```

- **Clawd 角色**：16 个动画按桌面状态切换（128×128，~5 FPS）。如果有自定义角色包，运行时换肤
- **状态徽章**：READY / WORKING / THINKING / DONE / ERROR / APPROVE? / OFFLINE 等
- **Mood / Energy / Fed / Level**：本地桌宠人格模拟值
  - Energy 5 档圆角格子，工作时慢掉，nap 时回血
  - Fed 连续条 + 10 等分刻度，按 tokens 进度填充
  - Level 中等字号，旁边箭头标 `-> L97 in 12K tok`
- **KPI 行**：`desk` / `today` / `rate` / `up`，紧凑格式（`445K` / `1.2M` / `12h05m`）
- **Recent activity**：最近 3 条；长按 KEY/BOOT 调出 8 条完整 history overlay

**长按 KEY/BOOT** → 全屏 history overlay 看完整 8 条 transcript。

### 2. USAGE（用量视图）

```
┌──────────────────────────────────────────┐
│ Plan usage limits   Max (5x)       17:00 │
├──────────────────────────────────────────┤
│ Current session                  12% used │
│ Resets in 4h 42m                          │
│ [████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░]   │
│                                           │
│ Weekly limits  BLE protocol doesn't expose│
│ All models                         n/a    │
│ Resets Thu 5:00 AM                        │
│ [░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░]   │
│                                           │
│ Sonnet only                        n/a    │
│ ─────────────────────────────────────     │
│ Authoritative (from BLE heartbeat)        │
│ session N tok  today N tok  midnight 4h.. │
└──────────────────────────────────────────┘
```

⚠️ **诚实标注**：
- 5h current session 用本地 token 滚动估算条
- weekly all / sonnet only 写 `n/a` —— 协议**不暴露**这些数据，不瞎编
- 底部 authoritative footer 写真值（来自心跳）

**长按 KEY/BOOT** → 打开**设置菜单**（见下）。

### 3. SYSTEM（系统诊断）

```
┌──────────────────────────────────────────┐
│ SYSTEM                  diagnostics      │
├──────────────────────────────────────────┤
│ Battery  4.09 V  93%  chip ESP32-S3      │
│ Climate  32.3°C  Humidity 45%  (SHTC3)   │
│ BLE      connected  hb 12s ago  NUS svc..│
│ Time     synced 97s ago  tz UTC+8 RTC:ok │
│ Uptime   0h 02m 46s  turns 17            │
│ Storage  fs 8K/11.88M  pack (built-in)   │
│ IMU      QMI8658 not detected            │
│ ─────────────────────────────────────    │
│ Heap     78.5 KB / 344.2 KB  (22%)       │
│ PSRAM    14.8 KB / 8.00 MB   (0%)        │
│ ─────────────────────────────────────    │
│ Velocity  avg 4s  min 1s  max 12s  (n=8) │
│ [▆ ▄ ▇ █ ▃ ▅ ▆ ▂]    oldest ... newest   │
│ Claude-desktop-buddy  built May 24 17:20 │
└──────────────────────────────────────────┘
```

- 完整硬件诊断 + 内存使用条
- 底部当有 approval velocity 数据时显示直方图，没有时显示一只 56×56 小 Clawd 来回走动

**长按 KEY/BOOT** → 切换**演示模式**（DEMO mode），跳到 MAIN 看 7 场景假心跳轮播。

### 4. CLOCK（大字时钟）

```
┌──────────────────────────────────────────┐
│ Clawd            □BT* WiFi 31°C  16:42 │
├──────────────────────────────────────────┤
│                                          │
│         ╔═════════════════╗              │
│         ║   16 : 42  :08  ║              │
│         ╚═════════════════╝              │
│                                          │
│      Sat, May 24 2026                    │
│                                          │
│      UTC+8:00   sync 12s ago (RTC)       │
│                                          │
│           [小 Clawd 巡逻]                 │
└──────────────────────────────────────────┘
```

logisoso50 巨字 HH:MM（冒号每秒闪一次），右侧小号 :SS，下方完整日期 +
时区 + 同步源（RTC / BLE）。还没同步时显示 `waiting for time sync` 不
伪造日期。

---

## 设置菜单

在 USAGE 视图长按 KEY 或 BOOT 进入：

| 菜单项 | 类型 | 作用 |
|---|---|---|
| **Sound** | toggle | 开/关 audio 反馈，持久化到 NVS |
| **Reset stats** | destructive | 清空 tokens / level / approvals / denies / turns / velocity；保留名字/声音/bonds/角色包 |
| **Remove packs** | destructive | 格式化 LittleFS，删除所有 `<name>/` 角色包目录 |
| **Factory reset** | destructive | 清 NVS + 清 BLE bonds + 格式化 LittleFS + 重启 |
| **Reboot** | destructive | 软重启，NVS 保留 |
| **Cancel / close** | nav | 退出菜单 |

destructive 项第一次 KEY 长按只弹**确认页**（三角警告 + 详情说明 +
`[KEY long] CONFIRM` / `[BOOT] CANCEL` 两大按钮），二次长按才真正执行。
菜单 30 秒无操作自动关闭。

---

## 自定义角色

### 方式 A：运行时推送（推荐）

从 Claude 桌面 app 直接推一个文件夹到设备，不用重新编译固件。

1. 在你的文件夹里放任意一组 GIF 命名为
   `idle.gif` / `building.gif` / `thinking.gif` / `happy.gif` /
   `notification.gif` / `error.gif` / `sleeping.gif` 等
   （文件名 → SpriteId 映射跟 `tools/gif_to_sprites.py` 一致，
   见 `src/pack.cpp::NAME_MAP`）
2. 桌面端 Hardware Buddy → 推送字符 / character pack
3. 推送完成后设备自动用 AnimatedGIF 解码到 PSRAM 1bpp 帧，替换 sprite
4. SYSTEM 视图 Storage 行会显示 `pack <name> (N/16)`
5. 设备重启会自动加载最近推送的 pack

不需要的 sprite 槽（比如只推 idle）会继续用编译期内置 sprite。

### 方式 B：编译期内置

`sprites.h` 是从 [clawd-on-desk](https://github.com/anthropics/clawd-on-desk)
项目的 GIF 自动转的。要替换内置角色：

1. 把你的 gif 放到 clawd-on-desk repo 的 `assets/gif/` 目录（或修改
   converter 脚本里的路径）
2. 改 `tools/gif_to_sprites.py` 顶部的 `SPRITES` 列表
3. 运行：
   ```bash
   python tools/gif_to_sprites.py
   ```
4. 调整 `src/ui.cpp::moodToSprite` 里的状态→sprite 映射
5. 重新 `pio run -t upload`

转换器逻辑：
- 取 8 帧均匀采样
- 跨帧并集 bbox 裁剪（保持位置稳定）
- 按 **彩色像素 = 墨水，纯白/纯黑/透明 = 背景** 二值化
  （这样身体填实，眼睛/嘴巴留洞）
- 等比缩放到 128×128，底部对齐（避免 CELEBRATE 类型动画错位）

---

## 文件结构

```
firmware-s3-rlcd/
├── platformio.ini          板子配置 (PSRAM + 自定义分区 + AnimatedGIF 依赖)
├── partitions_lfs.csv      16MB 分区表 (4MB app + 11.8MB LittleFS)
├── src/
│   ├── main.cpp            主循环 / energy ticker / 按键路由
│   ├── ble_nus.{h,cpp}     NimBLE NUS 外设 + LE SC pairing/bonding
│   ├── protocol.{h,cpp}    JSON 解析 / ack / 5h 滑窗 / 时间同步
│   ├── state.h             全局 BuddyState
│   ├── sensors.{h,cpp}     ADC 电池 + SHTC3 温湿度 + 充电检测
│   ├── rtc.{h,cpp}         PCF85063 BCD 读写
│   ├── persist.{h,cpp}     NVS 持久化 + resetStats
│   ├── xfer.{h,cpp}        folder push 五步状态机 → LittleFS
│   ├── pack.{h,cpp}        AnimatedGIF 运行时解码 → PSRAM sprite override
│   ├── audio.{h,cpp}       ES8311 + I2S，sin/expf 现场合成 ding/buzz
│   ├── imu.{h,cpp}         QMI8658C 驱动（本机 DNP 但代码就绪）
│   ├── menu.{h,cpp}        设置菜单状态机 + 6 项 dispatch
│   ├── demo.{h,cpp}        7 场景假心跳轮播
│   ├── buttons.{h,cpp}     KEY/BOOT 去抖 + 短/长按
│   ├── ui.{h,cpp}          U8g2 仪表盘 + 4 视图 + 多个全屏 overlay
│   ├── st7305_u8g2.{h,cpp} ST7305 ↔ U8g2 后端
│   └── sprites.h           生成的 128×128 1bpp 动画帧数据 (16 套)
├── tools/
│   └── gif_to_sprites.py   把 clawd-on-desk 的 gif 转成 PROGMEM
├── flash.bat               Windows 一键烧录
├── monitor.bat             Windows 一键看串口
└── CLAUDE.md               开发者 / agent 操作手册
```

---

## 协议覆盖范围

✅ 已实现：
- 心跳 snapshot 解析（total / running / waiting / msg / entries[8] / tokens /
  tokens_today / prompt）
- 命令 ack：`status` / `name` / `owner` / `unpair`
- `time` 时间同步 → 写入 PCF85063 RTC（跨重启不丢）
- **Folder push** 五步状态机（`char_begin` / `file` / `chunk` / `file_end` / `char_end`）
  → LittleFS → AnimatedGIF 解码 → PSRAM 替换 sprite
- **Permission 响应**：KEY=approve / BOOT=deny 全闭环，velocity ring buffer 记录响应时延
- **LE Secure Connections + MITM bonding**：屏上显示 6 位 passkey，链路加密
- BLE NUS 广播为 `Claude-<MAC tail>`，桌面端 picker 自动过滤

❌ 暂未实现（明确不做）：
- `turn` 事件接收（4KB 上限，价值低）

完整协议规范见父仓库 [`../REFERENCE.md`](https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md)。
本固件实现对照详见 [PORTING.md](PORTING.md)。

---

## 已知限制

- **字体只支持 Latin-1**：心跳里的中文 transcript 中的非 ASCII 字节会替换成空格（不是 `?`），整行不丢弃，但中文 / emoji 不显示
- **5 小时配额是估算值**：协议不暴露真实订阅配额，分母靠假定（USAGE 视图明确标注）；weekly 真值显示 `n/a` 不瞎编
- **第三个按键软件读不到**：电源键直接接 ETA6098 PMIC，没接 GPIO
- **IMU 没贴片**：你的板子 QMI8658C DNP，face-down nap 用 BLE 断开 5 min 兜底；驱动已写好，换块有 IMU 的同型号板插上就能用
- **GIF 解码阻塞主循环**：推角色包后 `pack::tick()` 同步解码会让 UI 卡顿几秒，目前可接受
- **KPI 都是 output token 数**：input/prompt token 不算（协议层不给）

---

## License

固件代码自由使用。`src/st7305_u8g2.{h,cpp}` 的 ST7305 初始化序列
和 U8g2 后端实现改编自 [Waveshare ESP32-S3-RLCD-4.2 官方 demo](https://github.com/waveshareteam/ESP32-S3-RLCD-4.2)。
Clawd 像素角色来自 Anthropic 的 [clawd-on-desk](https://github.com/anthropics/clawd-on-desk) 项目。

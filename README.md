# Claude Hardware Buddy · ESP32-S3-RLCD-4.2 固件

把 Waveshare ESP32-S3-RLCD-4.2 开发板变成一个 Claude 桌面 app 的
**Hardware Buddy**：在 4.2 英寸反射屏上画一只像素风 Clawd 桌宠，
显示当前会话状态、token 用量、温湿度、电量等信息，按键切换三个仪表盘视图。

通过蓝牙 LE 的 Nordic UART Service 协议跟 Claude for macOS / Windows
桌面客户端的 **Help → Troubleshooting → Enable Developer Mode →
Developer → Open Hardware Buddy…** 配对。

---

## 它能干什么

- 把 Claude 的实时状态显示在桌面摆件上 —— running / waiting / done / approve?
- Clawd 像素角色根据状态切换 8 种动画（idle / building / typing / thinking /
  happy / notification / error / sleeping）
- 三套仪表盘视图，按 KEY 下一屏 / BOOT 上一屏
- 板载 SHTC3 显示室内温湿度，PCF85063 RTC 同步桌面端时间，18650 电池电量
- 协议层完整 ack：`status` / `name` / `owner` / `unpair`
- 设备空闲时自动进入 DEMO 演示模式，轮播所有动画

---

## 硬件清单

| 模块               | 详情                                          |
|--------------------|-----------------------------------------------|
| 主控               | ESP32-S3-WROOM-1-N16R8 (16MB Flash / 8MB OPI PSRAM) |
| 屏幕               | 4.2" 反射式 LCD 300×400，ST7305 控制器          |
| 蓝牙               | BLE 5.0，Nordic UART Service                  |
| 温湿度             | SHTC3                                         |
| 实时时钟           | PCF85063                                      |
| 音频               | ES8311 codec（本固件未使用）                    |
| 电池               | 18650 锂电池座 + ADC1_CH3 分压采样             |
| 存储               | Micro SD 卡槽（本固件未使用）                   |
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

配对后桌面 app 会自动开始推送心跳，设备屏幕即时跟着变。

---

## 三个视图

按 **KEY** 下一屏，**BOOT** 上一屏。

### MAIN（主视图）

```
┌──────────────────────────────────────────┐
│ Claude-E658     □BT □WiFi  31°C  16:42   │
├──────────────────────────────────────────┤
│              │ Mood:   focused           │
│   [SPRITE]   │ Energy: ▓▓▓▓▓▓░░░  73%    │
│   Clawd      ├───────────────────────────┤
│   像素图      │ Now    2 session 3m 12s   │
│              │ session 142,213           │
│              │ today   223,401           │
│   [WORKING]  │ 5h       12,234           │
│              │ rate     142/min          │
│              │ turns       17  uptime 2h │
├──────────────────────────────────────────┤
│ Recent activity      msg: done, 13 turns │
│ > Bash: git status                       │
│ > Read: src/ui.cpp                       │
│ > Edit: src/main.cpp                     │
└──────────────────────────────────────────┘
```

- **Clawd 角色**：按状态切动画（128×128，~5 FPS）
- **状态徽章**：READY / WORKING / THINKING / DONE / ERROR / APPROVE? / OFFLINE
- **Energy / Mood**：本地"桌宠人格"模拟值。工作时 energy 慢慢掉，
  完成 turn +2 满足感奖励，离线睡觉快速回血。Mood 从 energy + 状态
  派生 (`focused / steady / weary / spry / content / drowsy / tired / asleep`)
- **6 项 KPI**：session/today/5h token 计数 + rate + turns + uptime
- **Recent activity**：桌面端推过来的最近 3 条 transcript

### USAGE（用量视图）

```
┌──────────────────────────────────────────┐
│ Plan usage limits  Max (5x) *      17:00 │
├──────────────────────────────────────────┤
│ Current session                  12% used │
│ Resets in 4h 42m                          │
│ [████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░]   │
│                                           │
│ Today                            18% used │
│ Resets in 6h 59m                          │
│ [█████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░]   │
│                                           │
│ Weekly limits  * local estimates          │
│ All models                       4% used  │
│ Resets Thu 5:00 AM                        │
│ [█░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░]   │
│                                           │
│ Sonnet only                      6% used  │
│ [██░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░]   │
│ ─────────────────────────────────────     │
│ raw: session X  today X  1h X  5h X       │
└──────────────────────────────────────────┘
```

四条进度条 + 详细数字。⚠️ **配额分母是本地估算的**（Max-5x 假定
5h=500K / today=1M / weekly=5M）—— 协议层目前不暴露 Anthropic
真实 5 小时滚动配额，所以这些 % 仅供参考。要真值需要桌面端在
heartbeat snapshot 里加 quota 字段。

### SYSTEM（系统诊断）

```
┌──────────────────────────────────────────┐
│ SYSTEM                  diagnostics      │
├──────────────────────────────────────────┤
│ Battery  4.09 V  93%  chip ESP32-S3      │
│ Climate  32.3°C  Humidity 45%  (SHTC3)   │
│ BLE      connected  hb 12s ago  NUS svc..│
│ Time     synced 97s ago  tz UTC+08:00    │
│ Uptime   0h 02m 46s  turns 17            │
│ Stats    energy 73%  mood: focused       │
│ ─────────────────────────────────────    │
│ Heap     78.5 KB / 344.2 KB  (22%)       │
│ [██░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░]   │
│                                          │
│ PSRAM    14.8 KB / 8.00 MB   (0%)        │
│ [░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░]   │
│                                          │
│             [🐰 走来走去的 Clawd]          │
│ ─────────────────────────────────────    │
│ Claude-desktop-buddy  built May 24 17:20 │
└──────────────────────────────────────────┘
```

底部有一只 56×56 的小 Clawd 在 16 秒周期里左右巡逻：building → happy →
thinking → sleeping。

---

## 文件结构

```
firmware-s3-rlcd/
├── platformio.ini          板子配置（启用 OPI PSRAM）
├── src/
│   ├── main.cpp            主循环 / energy ticker / 按键路由
│   ├── ble_nus.{h,cpp}     NimBLE NUS 外设，行缓冲，MTU 分片
│   ├── protocol.{h,cpp}    JSON 解析，ack，5h token 滑窗
│   ├── state.h             全局 BuddyState
│   ├── sensors.{h,cpp}     ADC 电池 + SHTC3 温湿度
│   ├── buttons.{h,cpp}     KEY/BOOT 去抖
│   ├── ui.{h,cpp}          U8g2 仪表盘 + 三视图
│   ├── st7305_u8g2.{h,cpp} ST7305 ↔ U8g2 后端
│   └── sprites.h           生成的 128×128 1bpp 动画帧数据
├── tools/
│   └── gif_to_sprites.py   把 clawd-on-desk 的 gif 转成 PROGMEM
├── flash.bat               Windows 一键烧录
├── monitor.bat             Windows 一键看串口
└── CLAUDE.md               开发者 / agent 操作手册
```

---

## 自定义角色

`sprites.h` 是从 [clawd-on-desk](https://github.com/anthropics/clawd-on-desk)
项目的 GIF 自动转的。要换成其他动画或角色：

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
- 等比缩放到 128×128，居中

---

## 协议覆盖范围

✅ 已实现：
- 心跳 snapshot 解析（total / running / waiting / msg / entries / tokens /
  tokens_today / prompt）
- 命令 ack：`status` / `name` / `owner` / `unpair`
- `time` 一次性同步
- `permission` 响应（接口保留，按键当前不用作 approve/deny）
- BLE NUS 广播为 `Claude-<MAC tail>`，桌面端 picker 自动过滤

❌ 暂未实现（按需可补）：
- `turn` 事件接收（含完整 SDK content）
- Folder push（角色包从桌面端拖拽推送）
- LE Secure Connections bonding（链路目前明文，私人环境用没问题，
  公共场所建议补上）

完整协议规范见父仓库 [`../REFERENCE.md`](https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md)。

---

## 已知限制

- **字体只支持 Latin-1**：心跳里的中文 transcript 会被替换成 `?`，
  含 `?` 的整行直接丢弃。加 CJK 字体大约要多占 150 KB flash。
- **5 小时配额是估算值**：协议不暴露真实订阅配额，分母靠假定。
- **第三个按键软件读不到**：电源键直接接 ETA6098 PMIC，没接 GPIO。
- **BLE 偶尔会断**：当前固件没做 LE Secure Connections bonding，
  Windows 可能会在一段时间后主动断开。下次断时看串口里
  `[ble] tx subscribe state=` 行帮助定位。

---

## License

固件代码自由使用。`src/st7305_u8g2.{h,cpp}` 的 ST7305 初始化序列
和 U8g2 后端实现改编自 [Waveshare ESP32-S3-RLCD-4.2 官方 demo](https://github.com/waveshareteam/ESP32-S3-RLCD-4.2)。
Clawd 像素角色来自 Anthropic 的 [clawd-on-desk](https://github.com/anthropics/clawd-on-desk) 项目。

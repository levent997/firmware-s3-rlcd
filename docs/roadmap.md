# Roadmap — firmware-s3-rlcd

对比参考项目 `xiaozhi-esp32`(小智 AI 语音助手,ESP-IDF)做的工程差距分析与改进计划。
本文档**实时更新实施进度**。

---

## 定位差异(对比前提)

|  | firmware-s3-rlcd(本项目) | xiaozhi-esp32(参考) |
|---|---|---|
| 角色 | 被动显示外设 — 显示 Claude 会话状态 | 主动 AI 语音助手 — 对话机器人 |
| 框架 | Arduino | ESP-IDF |
| 连接 | 纯 BLE(NUS,无网络) | WiFi + WebSocket/MQTT + 云 LLM |
| 交互 | 按键 approve/deny | 语音唤醒 + 全双工对话 |
| 显示 | U8g2 单色反射屏 | LVGL 彩色/多屏 |
| 板型 | 单板 | 70+ 板抽象层 |

**明确不抄(与"BLE 桌面伴侣"定位无关,抄了只会臃肿):**
音频对话管线(Opus/唤醒词/AEC/VAD)、WiFi/WebSocket/MQTT/云后端、设备激活、
MCP 云控制、LVGL。

---

## 值得借鉴的工程差距

| # | 能力 | xiaozhi 做法 | 本项目现状 | 价值 | 成本 |
|---|---|---|---|---|---|
| 1 | 电源管理 | SetPowerSaveLevel + sleep_timer + 深睡 | 仅 CPU 降频 + 慢广播 | 高 | 低 |
| 2 | 日志分级 | ESP_LOGx + tag + 编译期级别 | 裸 Serial.printf | 中 | 低 |
| 3 | 主机侧单测 | (主要靠 CI build) | 零测试 | 高 | 中 |
| 4 | CI + clang-format | GitHub Actions 矩阵构建 + .clang-format | 无 | 中 | 低 |
| 5 | 通用设置框架 | Settings 类 key-value + 命名空间 | persist.cpp 每字段硬编码 | 中 | 中 |
| 6 | 看门狗/健壮性 | 状态机错误态 + OTA 回滚 | 无 task watchdog | 中 | 低 |
| 7 | OTA 升级 | 双槽 HTTP OTA + 回滚 | 纯 USB 烧录 | 中高 | 高 |
| 8 | 中文显示 | 25+ 语言 + Puhui CJK | 仅 Latin-1 | 中 | 中 |
| 9 | 板级抽象 | 70+ 板 Board 基类 | 单板硬编码 | 低中 | 高 |

---

## 分阶段计划与进度

状态图例:⬜ 未开始 / 🔵 进行中 / ✅ 完成 / ⏸ 暂缓

### 阶段 1 — 续电池主题,高价值低成本

- 🔵 **1.1 light-sleep** — 主循环空闲进 light-sleep / esp_pm 自动 light-sleep,降空闲电流
- ⬜ **1.2 屏超时 + 断连深睡** — 无交互 N 分钟停 LCD 刷新;BLE 断连超时进 deep-sleep,按键唤醒
- ⬜ **1.3 日志分级** — LOG_LEVEL 宏,release 编译期裁掉 debug 打印

### 阶段 2 — 质量与健壮性

- ⬜ **2.1 主机侧单测** — `[env:native]`,测 protocol 解析 / `lipoSocFromV` / 充电判定 / token 窗口
- ⬜ **2.2 CI** — GitHub Actions:`pio run` + `pio test -e native` + clang-format dry-run
- ⬜ **2.3 看门狗** — task watchdog;BLE 回调 / GIF 解码超时保护

### 阶段 3 — 架构改进(可选)

- ⬜ **3.1 通用 Settings 封装** — persist.cpp 重构成 key-value 封装
- ⏸ **3.2 BLE OTA** — NimBLE DFU 推固件(需评估桌面端依赖,受 CLAUDE.md "不要求桌面端加字段"约束)
- ⬜ **3.3 中文字体** — U8g2 unifont_t_chinese 子集,温湿度/日期/菜单中文化

### 阶段 4 — 长期(大重构,收益待定)

- ⏸ **4.1 板级抽象** — 统一 M5StickC(claude-desktop-buddy)与 S3 两套固件到一个 Board 基类

---

## 变更日志

| 日期 | commit | 内容 |
|---|---|---|
| 2026-05-28 | `430cba5` | 基线:电池 SOC 精度 + 充电冻结 + 降功耗 + CLOCK 视图重设计 |

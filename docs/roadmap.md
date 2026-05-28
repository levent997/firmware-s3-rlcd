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

- ✅ **1.1 light-sleep** — `esp_pm_configure` 自动 light-sleep(DFS 40-80MHz + tickless idle,与 BLE 控制器协调)。Arduino 2.0.17 若 `CONFIG_PM_ENABLE` 关则 no-op 并打日志,固定 80MHz
- ✅ **1.2 断连深睡** — BLE 断连 **且** 无按键 > `IDLE_DEEP_SLEEP_MS`(默认 30min,0=关)进 deep-sleep,睡前画 "Sleeping" 屏,KEY(GPIO18 ext1 ALL_LOW)唤醒后重启从 NVS/RTC 恢复
- ✅ **1.3 日志分级** — `src/log.h` 的 `LOGE/LOGI/LOGD` + `LOG_LEVEL` 编译期裁剪;`[hb]`/`<-` 改 LOGD;platformio.ini 注明 release 用 `-DLOG_LEVEL=1`

> 说明:阶段 1 未做"awake 时屏超时"。反射屏无背光、靠内容保持,冻结时钟会显示过期时间反而困惑;改为靠 1.1 自动 light-sleep 降 awake 空闲功耗 + 1.2 长时间断连直接深睡。

### 阶段 2 — 质量与健壮性

- 🔵 **2.1 主机侧单测** — `[env:native]` + Unity 已落地。把 SOC 曲线 + 充电判定状态机抽到无 Arduino 依赖的 `src/battery_math.{h,cpp}`(`BatteryEstimator`),`sensors.cpp` 改为委托调用。`test/test_battery/` 7 个用例全过,含核心的"插电 SOC 冻结"回归测试。主机编译器:WinLibs MinGW g++ 16.1.0(winget 用户作用域)。**待补**:protocol 解析 / token 窗口的测试
- ✅ **2.2 CI** — `.github/workflows/build.yml`:`pio run -e esp32-s3-rlcd-42`(硬门)+ `pio test -e native`(硬门)+ clang-format(advisory,`continue-on-error`,排除生成的 sprites.h)。`.clang-format` = LLVM 基底 2 空格/100 列/指针右贴,贴近现有风格
- ✅ **2.3 看门狗** — 软件看门狗(`main.cpp`):core0 监控任务,`loop()` 每轮戳 `g_loop_alive_ms`,停摆 > 30s 则 `esp_restart()`。超时取 30s 以免误杀阻塞数秒的 GIF 解码。未用 IDF 4.4 的 TWDT(其 5s 超时被启动代码锁定、改不动)

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
| 2026-05-28 | `3534438` | docs: 新增 roadmap.md |
| 2026-05-28 | (本次) | 阶段 1 完成:自动 light-sleep + 断连深睡 + 日志分级。已烧录 COM3 |
| 2026-05-28 | (本次) | 阶段 2.1:抽取 battery_math + BatteryEstimator,加 native env + 7 个 Unity 单测(全过)。固件重构后编译通过、已烧录 |
| 2026-05-28 | (本次) | 阶段 2.2 + 2.3:GitHub Actions CI(build+test 硬门 / clang-format advisory)+ .clang-format;软件看门狗(core0,30s)。测试通过、固件已烧录 |

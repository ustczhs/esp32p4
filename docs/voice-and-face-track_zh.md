# 语音对话与本地人脸追踪实现说明

本文记录当前板级固件中 **语音（小智云端）** 与 **本地人脸追踪** 的实现方式。二者同时运行，且可独立开关。

适用硬件：微雪 ESP32-P4-WIFI6-Touch-LCD-4.3（`esp32-p4x-wifi6-touch-lcd-4.3`），OV5647 MIPI 摄像头，32MB Flash。固件基于 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) + ESP-IDF 6.1。

---

## 1. 总体架构

```
麦克风 ES7210 --> AFE（AEC + VAD + WakeNet）--> Opus --> WebSocket --> xiaozhi.me
扬声器 ES8311 <-- 解码 / 播放 <--------------------------------┘

OV5647 MIPI --> EspVideo GrabFrame --> 240x240 下采样 --> ESP-DL MSR+MNP
                                          |
                                          +--> 屏幕通知栏坐标  face (x,y) / (nan,nan)
                                          +--> 左下角低分辨率预览（可关）
```

设计原则：

- **语音走云端**：设备只做前端处理与编解码，对话在 `xiaozhi.me`。
- **人脸在本地**：不上传视频流；屏幕不显示完整人脸画面，默认只显示中心点坐标。
- **互不阻塞**：人脸任务钉在 CPU1，语音 AFE / Opus 走原有音频路径。
- **可独立关闭**：MCP 工具 `self.audio.set_enabled` 与 `self.face_track.set_enabled`。

云端视频分析、WebRTC 推流尚未实现。

---

## 2. 语音实现

### 2.1 硬件与前端

| 模块 | 实现 |
|------|------|
| 输入 | ES7210 TDM，24kHz，多麦；软件重采样到 16kHz 再喂 AFE |
| 输出 | ES8311 I2S，24kHz 播放 |
| 唤醒 | ESP-SR WakeNet9（「你好小智」） |
| 处理 | ESP-SR AFE：AEC（FD_LOW_COST）+ WebRTC VAD + WakeNet |

核心代码：

- `main/audio/audio_service.cc`：唤醒 / 听 / 说状态切换
- `main/audio/engines/afe_audio_engine.cc`：AFE 创建、喂数、取结果
- `main/boards/waveshare/esp32-p4-wifi6-touch-lcd/`：编解码器与 I2S 引脚

AFE 典型流水线（串口日志）：

```
[input] -> |AEC(FD_LOW_COST, NLP_VERY_AGGRESSIVE)| -> |VAD(WebRTC)| -> |WakeNet(wn9_你好小智)| -> [output]
```

设备 AEC 由板级配置打开：`CONFIG_USE_DEVICE_AEC=y`。

### 2.2 云端会话

不是 WebRTC。会话走小智官方 **WebSocket + Opus**：

1. 设备联网后 MQTT 上报，再按需打开音频通道。
2. `WebsocketProtocol::OpenAudioChannel()` 连接服务器，发送 `hello`（`audio_params.format = opus`）。
3. 上行：麦克风 PCM --> Opus 编码 --> WebSocket 二进制帧。
4. 下行：Opus --> PCM --> ES8311 播放。
5. 打断：播放时仍采集；VAD / 唤醒命中后停播或切状态。

协议细节见 [`websocket_zh.md`](./websocket_zh.md)。MCP 物联网控制见 [`mcp-protocol_zh.md`](./mcp-protocol_zh.md)。

### 2.3 运行时开关

`FaceTracker::Start()` 注册：

```
self.audio.set_enabled  { "enabled": true|false }
```

- `true`：打开唤醒检测。
- `false`：关闭语音处理与唤醒；若正在听/说，切到 Idle。
- 写入 NVS 命名空间 `audio`，与人脸开关分离。

人脸追踪不受该开关影响。

### 2.4 状态机（简表）

`Application` 中与音频相关的状态：

| 状态 | 行为 |
|------|------|
| Idle | 唤醒开启，等待「你好小智」 |
| Listening | 关唤醒、开语音处理，上行 Opus |
| Speaking | 播放下行，可被打断 |
| Connecting | 打开 WebSocket 音频通道 |

实现入口：`Application::SetDeviceState()`（`main/application.cc`）。

---

## 3. 本地人脸追踪

### 3.1 编译开关与依赖

仅 ESP32-P4：

```
CONFIG_XIAOZHI_ENABLE_FACE_TRACK=y
```

定义于 `main/Kconfig.projbuild`。`main/CMakeLists.txt` 在该宏打开时编译 `face_tracker.cc`。

依赖（`main/idf_component.yml`，仅 P4）：

```
espressif/human_face_detect
```

默认模型：**MSRMNP_S8_V1**（MSR + MNP），打进 Flash RO 数据。不打包更重的 ESPDET_PICO。

板级追加配置见：

- `main/boards/waveshare/esp32-p4-wifi6-touch-lcd/config.json`（`esp32-p4x-wifi6-touch-lcd-4.3`）
- `sdkconfig.defaults.esp32p4`

### 3.2 启动时机

激活 / OTA 检查完成后：

```cpp
#if CONFIG_XIAOZHI_ENABLE_FACE_TRACK
    FaceTracker::GetInstance().Start();
#endif
```

位于 `Application` 激活收尾（`main/application.cc`）。**无需按键启动**。上电联网约 2 秒后开始检测。

`Start()` 会：

1. 从 NVS `face` 读取 `enabled` / `preview`（默认均为 true）。
2. 注册 MCP 工具。
3. 创建任务 `face_track`：栈 8KB，优先级 3，钉在 **CPU1**。

### 3.3 检测循环

文件：`main/face_tracker.cc`。

1. `Camera::GrabFrame(&frame, false)` 取一帧（`false` = 不走拍照气泡预览，避免 800x800 全图转码）。
2. 传感器格式为 **UYVY 800x800**。本板打开了 `CONFIG_XIAOZHI_ENABLE_CAMERA_ENDIANNESS_SWAP`，`GrabFrame` 对每个 `uint16_t` 做 `bswap16`，内存布局变为 **YUYV**。
3. 按 **YUYV** 采样并缩放到 **240x240 RGB565**（PSRAM）。
4. `HumanFaceDetect(MSRMNP_S8_V1)` 推理。
5. 取面积最大的人脸，把框中心映射回 800x800 坐标，指数平滑（alpha=0.35）。
6. 周期约 **250ms**（约 4fps）；单帧实测约 **110-135ms**（含取帧 + 下采样 + 推理）。

坐标显示：

- 有脸：状态栏 `face (x,y)`，串口 `FaceTracker: face center=(x,y) score=...`
- 无脸：`face (nan,nan)`（MCP 的 `cx`/`cy` 为 `null`）

### 3.4 调试预览（低分辨率）

`LcdDisplay::SetDebugPreviewRgb565()`（`main/display/lcd_display.cc`）：

- 左下角常驻约 160px 窗口，内容即检测用的 240x240 图。
- 使用 `lv_canvas` + 64 字节对齐的 PSRAM 缓冲，写入后 `esp_cache_msync`（C2M），避免 PSRAM 缓存导致黑窗。
- 有脸画绿框/十字；无脸画红十字，便于确认画面是否在动。
- 可关：`self.face_track.set_preview`。

实现注意：

- **不要**在无人脸时改用另一种 YUV 排布再检测：错误排布会把绿/紫偏色留在预览上。
- 预览缓冲按 RGB565 直接拷贝给 LVGL，不再二次 endian swap（摄像头路径已经 swap 过）。

### 3.5 摄像头接口

`EspVideo`（`main/boards/common/esp_video.cc`）：

- `GrabFrame` 带 `capture_mutex_`，与拍照 `Explain` 互斥。
- 从 V4L2 DQBUF 拷到 PSRAM（约 800x800x2 ≈ 1.28MB）。
- 人脸路径 `update_preview=false`，不创建聊天区全图气泡。

### 3.6 MCP 工具

| 工具 | 作用 |
|------|------|
| `self.face_track.set_enabled` | 开/关本地追踪 |
| `self.face_track.set_preview` | 开/关左下角小窗 |
| `self.face_track.get_status` | JSON：`enabled` / `preview` / `has_face` / `cx` / `cy` / `frame_w` / `frame_h` |
| `self.audio.set_enabled` | 开/关语音管线 |

`get_status` 示例：

```json
{"enabled":true,"preview":true,"has_face":true,"cx":454,"cy":398,"frame_w":800,"frame_h":800}
```

无人脸时 `cx`、`cy` 为 `null`。

---

## 4. 分区与编译

人脸模型 + ESP-DL 使应用超过原 16MB 表的 4MB OTA 槽。本板 Flash 为 32MB，使用：

`partitions/v2/32m_face.csv`

| 分区 | 大小 |
|------|------|
| ota_0 / ota_1 | 各 7MB |
| assets | 10MB |

`config.json` 中同时设置 `CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y`。

编译示例：

```bash
source /home/zhs/esp/esp-idf-v6.1/export.sh
cd xiaozhi-esp32
python3 scripts/build.py waveshare/esp32-p4-wifi6-touch-lcd \
  --name esp32-p4x-wifi6-touch-lcd-4.3 --language zh-CN
```

烧录（串口 `/dev/ttyACM0`）：

```bash
cd build
python -m esptool --chip esp32p4 -p /dev/ttyACM0 -b 460800 \
  --before default-reset --after hard-reset write-flash @flash_args
```

---

## 5. 资源占用（实测，语音+追踪+预览同时开）

| 项 | 数值 |
|----|------|
| `xiaozhi.bin` | 约 6.46MB / 7MB OTA |
| 人脸模型包 | 约 191KB（MSR 60KB + MNP 127KB） |
| 检测缓冲 | 240x240 RGB565 ≈ 113KB PSRAM |
| 预览拷贝 | 再约 113KB PSRAM |
| 摄像头整帧 | ≈ 1.28MB PSRAM（原采集路径） |
| `face_track` 栈 | 8KB |
| 片内 SRAM 剩余 | 约 122KB（最低约 115KB） |
| CPU1 | 每 250ms 占用约 110-135ms（约 45%-55%） |
| 检测帧率 | 约 4fps |

关闭预览可省一块 PSRAM 和 LVGL 刷新；改回 160x160 可再降推理时间。

---

## 6. 关键文件

| 文件 | 内容 |
|------|------|
| `main/face_tracker.cc` / `.h` | 追踪任务、MCP、YUV 采样、检测 |
| `main/application.cc` | 激活后 `FaceTracker::Start()`；语音状态机 |
| `main/audio/engines/afe_audio_engine.cc` | AFE / 唤醒 |
| `main/protocols/websocket_protocol.cc` | 云端 Opus 会话 |
| `main/boards/common/esp_video.cc` | 取帧、endian swap、互斥 |
| `main/display/lcd_display.cc` | 调试小窗 canvas + cache sync |
| `partitions/v2/32m_face.csv` | 32MB / 7MB OTA |
| `main/boards/waveshare/esp32-p4-wifi6-touch-lcd/config.json` | 板级 sdkconfig 追加项 |

---

## 7. 使用与排障

- **怎么启动**：上电即可；屏幕顶部应出现 `face (nan,nan)` 或 `face (x,y)`。
- **怎么看状态**：屏幕通知栏、串口 `FaceTracker` 日志、MCP `self.face_track.get_status`。
- **小窗全黑**：检查 PSRAM `esp_cache_msync` 与 canvas 缓冲对齐。
- **绿紫偏色**：YUV 排布与 `bswap16` 不匹配；本板应对 UYVY 帧按 **YUYV** 采样。
- **很难检出**：看左下角构图、亮度、距离（约 0.5-1.5m 正对镜头）。

语音与人脸默认同时开。只需语音时关追踪：`self.face_track.set_enabled`；只需追踪时关语音：`self.audio.set_enabled`。

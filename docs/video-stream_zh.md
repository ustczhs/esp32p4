# 局域网 H.264 预览推流

摄像头经 **CameraHub 一帧多用**，硬件 H.264 编码后以 HTTP 分块推送 Annex-B，可用 ffplay / VLC 观看。

## 怎么查看视频流

设备与电脑需能互通（同一局域网，且路由/AP 未开「无线隔离」）。

当前常见拓扑：
- 板子：Wi-Fi `TCL-WiFi6`，IP 如 `172.16.208.15`
- 有线电脑：可能是 `172.16.40.x`（跨网段，有时通有时超时）

**更稳妥**：用手机热点/笔记本连同一 `TCL-WiFi6` 再播；或电脑也连该 Wi-Fi。

串口出现 `Video stream ON -> http://<IP>:8080/stream.h264` 后，先测：

```bash
ping -c 3 <设备IP>
curl -m 3 -o /tmp/t.h264 http://<设备IP>:8080/stream.h264 && ls -l /tmp/t.h264
```

`ping` 不通就先别开 ffplay（网络层不通）。

### 1. ffplay（推荐）

```bash
sudo apt install ffmpeg
ffplay -fflags nobuffer -flags low_delay -f h264 -i http://172.16.208.15:8080/stream.h264
```

### 2. VLC

```bash
vlc --demux h264 http://172.16.208.15:8080/stream.h264
```

### 3. 确认流是否在跑

串口应出现类似日志：

```text
VideoStreamer: ReserveEncoderEarly: ...
VideoStreamer: H264 encoder 800x800 @12fps 2500000bps ...
VideoStreamer: Video stream ON -> http://<IP>:8080/stream.h264 (800x800 @2500000bps)
VideoStreamer: encoded=...
```

也可用 MCP：`self.video.get_status`（含 `width`/`height`/`bitrate`/URL）。

关闭：`self.video.set_enabled { "enabled": false }`

## 分辨率与画质（如何提高）

硬件 H.264 的 **参考帧缓冲区必须在 INTERNAL SRAM**（约 57KB@800 / 46KB@640 / 35KB@480）。
AFE 语音引擎也会占用连续 INTERNAL，因此编码分辨率按内存自动降级：

| 优先级 | 编码分辨率 | 码率 | 说明 |
|--------|------------|------|------|
| 1 | **800×800** | 1.2 Mbps | 传感器全分辨率（码率受 2.4G Wi-Fi 限制） |
| 2 | 640×640 | 1.0 Mbps | 全幅缩放到 640（保留视野，非中心裁切） |
| 3 | 480×480 | 0.8 Mbps | 兜底 |

实现要点（已在固件中）：
1. **`ReserveEncoderEarly()`**：在 AFE 初始化之前打开编码器，抢占 INTERNAL。
2. **预设阶梯**：`kPresets[]` 在 `main/video_streamer.cc`（改这里即可换目标分辨率/码率）。
3. **全幅缩放**：`ScaleYuyvNearest`，避免旧版中心裁切丢边。
4. 关闭推流时 **保留编码器**，避免再次 OOM 掉到低分辨率。

若日志里是 `640x640` 而不是 `800x800`，说明开机时最大连续 INTERNAL 仍不够 57KB——可再检查是否有其它模块提前占内存。

## 启用方式

1. 固件打开 `CONFIG_XIAOZHI_ENABLE_VIDEO_STREAM`（本板已开）。
2. 默认 **Wi-Fi 连上后自动开启**；也可 MCP `self.video.set_enabled`。

## 默认参数

| 项 | 值 |
|----|-----|
| 采集 | 800×800 |
| 编码 | 尽量 800，否则 640 / 480 |
| 帧率 | ~12 fps |
| GOP | 12 |
| 端口 / 路径 | `8080` / `/stream.h264` |
| 客户端 | 同时 1 路 |

## 数据流

```
OV5647 → EspVideo GrabFrame → CameraHub（PSRAM 拷贝 + seq）
                                  ├→ FaceTracker（缩到 240×240 检测 → Release）
                                  └→ VideoStreamer（缩放到 enc → UYVY → HW H.264 → HTTP）
```

## 排查

| 现象 | 原因 / 处理 |
|------|-------------|
| Connection timed out | 跨网段/AP 隔离；改连同一 Wi-Fi |
| 分辨率只有 480 | INTERNAL 不够；看 `ReserveEncoderEarly` / `OpenEncoder` 日志 |
| busy: one client only | 关掉旧的 ffplay/VLC |

| 播放卡死后再连超时 | 高码率堵死 Wi-Fi TX；已降到约 1.2Mbps@800、发送短超时、不持锁发送。先 `ping`，不通就复位板子 |

## 限制

- 依赖局域网 Wi-Fi（P4↔C6 Hosted）。
- 第一版无 RTSP / MSE / WebRTC。

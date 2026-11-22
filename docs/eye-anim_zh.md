# 双眼 GIF 表情实现说明

本文记录当前板级固件中 **上半屏双眼表情动画** 的设计、素材、播放路径、切换入口与线程安全约束。

适用硬件：微雪 ESP32-P4-WIFI6-Touch-LCD-4.3（`esp32-p4x-wifi6-touch-lcd-4.3`）。仅 ESP32-P4 目标可打开该功能。语音与人脸追踪的总览见 [voice-and-face-track_zh.md](./voice-and-face-track_zh.md)。

尚未实现：瞳孔跟随人脸（gaze）。

---

## 1. 设计结论

| 决策 | 选择 | 原因 |
|------|------|------|
| 布局 | 左右并排，各 200×200，总区域 400×200 | 放在状态栏 / `face (x,y)` 文字下方、居中 emoji 上方 |
| 素材数量 | **一份左眼 GIF**，右眼运行时水平镜像 | 不双份 GIF、不预合成双眼图，assets 体积约一半 |
| 填充方式 | 离线 crop-to-fill 成正方形 | 运行时只解码、不缩放 |
| 播放器 | 全局单例 `EyePlayer` + 一个 `LvglGif` | 同时只播一条 clip，切换即销毁旧解码器 |
| 存放位置 | assets 分区，不进 OTA app | GIF 约 749KB，不膨胀 `xiaozhi.bin` |

数据流：

```
main/assets/eyes/{blink,happy,excited}.gif
        │  构建时打进 generated_assets.bin（assets 分区 mmap）
        ▼
   EyePlayer::LoadClip
        │  LvglGif 解码 → 200×200 ARGB8888 canvas
        ▼
   LcdDisplay::SetEyeFrame
        │  左眼 lv_image 直接引用 canvas
        │  右眼逐像素水平镜像到 PSRAM 缓冲后再设 src
        ▼
   上半屏 400×200 双眼 overlay
```

---

## 2. 编译开关与素材

Kconfig（`main/Kconfig.projbuild`）：

```
CONFIG_XIAOZHI_ENABLE_EYE_ANIM=y
```

本板在 `main/boards/waveshare/esp32-p4-wifi6-touch-lcd/config.json` 中默认打开。关闭后不编译 `eye_player.cc`，也不打包眼睛 GIF。

`main/CMakeLists.txt` 在开关打开且非 ESP-HI 板时：

```cmake
set(DEFAULT_ASSETS_EXTRA_FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/eyes")
```

目录 `main/assets/eyes/` 下三份 GIF 会进入 `generated_assets.bin`。改 GIF 后需要 **重烧 assets**（`flash_args` 里的 `generated_assets.bin`），只烧 app 看不到新素材。

### 2.1 目前支持的表情（3 种）

| 规范名 | GIF | 含义 | 播放策略 |
|--------|-----|------|----------|
| `blink` | `blink.gif` | 待机眨眼 | 无限循环 |
| `happy` | `happy.gif` | 开心 / 笑 | 约 2 遍后自动回 `blink` |
| `excited` | `excited.gif` | 兴奋 / 激动 | 约 2 遍后自动回 `blink` |

约定：各 clip **200×200、约 12fps、约 48 帧**。`happy` / `excited` 回待机延时：

```
kClipDurationMs = 4010   // ~48 帧 × 12fps
kHappyExcitedLoops = 2   // 约 8s
```

源片来自 aime1 整脸素材，离线裁左眼并充满方框；固件不存右眼 GIF。

---

## 3. 屏幕布局

`LcdDisplay::EnsureEyeUi()` 创建浮层：

- 容器 `eye_box_`：400×200，`LV_ALIGN_TOP_MID`，竖直偏移约为正文字高 + spacing。
- `eye_left_` / `eye_right_`：各 200×200 的 `lv_image`，横向 flex 居中。
- 层级：眼睛在 emoji 之上，状态栏 / 顶栏仍在最前。
- 眼睛可见时隐藏居中 `emoji_box_`；`ClearEyes()` 时恢复 emoji。
- 拍照预览等路径可暂藏眼睛（与 emoji 互斥显示）。

右眼缓冲：`heap_caps_aligned_alloc(64, w*h*4, MALLOC_CAP_SPIRAM)`，约 160KB；解码 canvas 另约 160KB。写完镜像后 `esp_cache_msync(..., C2M)`，避免 PSRAM 花屏。

---

## 4. 核心类与生命周期

### 4.1 `EyePlayer`（`main/eye_player.h` / `.cc`）

单例。启动时机：`Assets::Apply()` 之后（assets mmap 必须先成功），见 `application.cc`：

```cpp
assets.Apply();
EyePlayer::GetInstance().Start();
```

`Start()`：

1. 读 NVS 命名空间 `eyes` 的 `enabled`（默认 true）。
2. 创建 `esp_timer`（`eye_return`），仅用于 **排队** 回 `blink`，回调里不解码。
3. 注册 MCP。
4. 若启用则 `SetExpr("blink", true)`。

开关持久化：`EyePlayer::SetEnabled` → `Settings("eyes")` 的 `enabled`。

### 4.2 `LoadClip`（切换真正发生的地方）

1. `Assets::GetAssetData("blink.gif" | "happy.gif" | "excited.gif")` 取 mmap 指针（零拷贝）。
2. **持 `DisplayLockGuard`（LVGL 递归锁）**。
3. `DetachEyeImageSrc()`：左右眼 `lv_image_set_src(nullptr)`，避免 LVGL 仍指向即将释放的 canvas。
4. `gif_->Stop(); gif_.reset();` 删除旧 `lv_timer` 与 GIF 解码器。
5. `LvglRawImage` + `LvglGif` 打开新 clip，渲染首帧。
6. `SetFrameCallback` → `OnFrame()` → `SetEyeFrame`（左眼 src + 右眼镜像）。
7. `gif_->Start()` 创建 / 恢复 10ms 的 LVGL timer，按 GIF GCE delay 推进。

`LvglGif`（`main/display/lvgl_display/gif/lvgl_gif.cc`）与 emoji GIF 共用：canvas 为 ARGB8888，`lv_timer` 回调 `NextFrame()`。

### 4.3 别名与云端情绪映射

`SetExpr` 先 `NormalizeExpr`：

| 输入 | 落到 |
|------|------|
| `happy` / `smile` / 开心 / 笑 | `happy` |
| `excited` / `high` / 兴奋 / 激动 | `excited` |
| `blink` / `idle` / `neutral` / 眨眼 / 待机 | `blink` |
| 其它未知字符串 | `blink` |

对话情绪走 `SetExprFromEmotion`（云端 `llm.emotion`、`Alert`、回到 idle 时的 `neutral`）：

| 眼睛 clip | 云端 emotion 示例 |
|-----------|-------------------|
| `happy` | happy, laughing, funny, loving, kissy, cool, confident, delicious, winking, relaxed |
| `excited` | surprised, shocked, angry, embarrassed, silly |
| `blink` | 其余（neutral, sad, sleepy, thinking, confused, crying, …） |

`SetExpr(..., force=false)` 时若目标与当前相同则跳过重载，避免每条 emotion 都重建 GIF。

---

## 5. 切换入口

| 方式 | 路径 | 说明 |
|------|------|------|
| BOOT 双击 | `boot_button_.OnDoubleClick` → `RequestCycleNext()` | 循环 `blink → happy → excited → blink`，顶栏 `eyes: xxx`。单击仍是 `ToggleChatState` |
| 语音 | 云端调 MCP `self.eyes.set_expr` / `self.eyes.next` | 例如「眼睛开心一点」「换个眼睛表情」 |
| 对话情绪 | `application.cc` 处理 `llm` JSON 的 `emotion` | 自动映射，不弹通知 |
| 告警 / 回待机 | `Alert()` / idle 的 `neutral` | 同样走 `SetExprFromEmotion` |
| MCP 开关 | `self.eyes.set_enabled` / `get_status` | UserOnly；关则拆 GIF 并显示 emoji |

MCP（对云端可见）：

- `self.eyes.set_expr`：参数 `expr` 字符串。
- `self.eyes.next`：等同 `CycleNext()`。

MCP（仅本机 / 调试）：

- `self.eyes.set_enabled`
- `self.eyes.get_status`：`{"enabled":true,"expr":"blink"}`

---

## 6. 线程安全（切换时曾重启的原因）

**不是空闲 OOM。** 空闲片内 SRAM 约 126KB，PSRAM 仍有十余 MB。重启来自错误线程上销毁 / 创建 GIF。

| 错误做法 | 后果 |
|----------|------|
| 按钮任务里直接 `CycleNext()` → `LoadClip` → `gif_.reset()` | LVGL 任务仍在 `OnFrame` / 刷屏，UAF |
| `esp_timer` 回调里 `SetExpr("blink")` 解码 GIF | `CONFIG_ESP_TIMER_TASK_STACK_SIZE=3584`，栈溢出 |

现行约束：

1. **所有 `lv_timer` 的创建 / 删除、改 `lv_image` src，必须在 `DisplayLockGuard` 内**（与 `LcdDisplay::SetEmotion` 的 emoji GIF 相同）。`lvgl_port_lock` 是递归锁，`LoadClip` 持锁后再进 `SetEyeFrame` 合法。
2. **BOOT 双击只 `Application::Schedule`**：`RequestCycleNext()` 丢到主循环再 `CycleNext()`。
3. **回 blink 的 timer 回调也只 `Schedule`**，不在 `esp_timer` 任务里 `LoadClip`。
4. 销毁前先 `DetachEyeImageSrc()`，避免 LVGL 仍引用旧 descriptor。

切换成功时串口类似：

```
I (...) EyePlayer: Playing eye clip: happy (happy.gif, 200x200) free_sram=... free_psram=...
I (...) EyePlayer: CycleNext -> happy
```

若再重启，优先看 `Guru Meditation` / `backtrace` 是否仍落在 `LvglGif` / `lv_timer`。

---

## 7. 资源占用（量级）

| 项 | 数值 |
|----|------|
| 三份 GIF | 约 749KB，assets 分区 |
| 解码 canvas | 200×200×4 ≈ 160KB（LVGL / GIF 库分配） |
| 右眼镜像 | 再约 160KB PSRAM |
| CPU | 每帧水平镜像 200×200 + LVGL 刷新；无独立 FreeRTOS 任务 |

---

## 8. 关键文件

| 文件 | 职责 |
|------|------|
| `main/eye_player.cc` / `.h` | clip 选择、MCP、回 blink、加锁加载 |
| `main/assets/eyes/*.gif` | 左眼素材 |
| `main/display/lcd_display.cc` | `EnsureEyeUi` / `SetEyeFrame` / 镜像 / `DetachEyeImageSrc` |
| `main/display/lvgl_display/gif/lvgl_gif.cc` | GIF 解码与 `lv_timer` |
| `main/application.cc` | `Start()`、emotion / Alert / idle 映射 |
| `main/boards/waveshare/esp32-p4-wifi6-touch-lcd/esp32-p4-wifi6-touch-lcd.cc` | BOOT 双击 |
| `main/CMakeLists.txt` | `DEFAULT_ASSETS_EXTRA_FILES` |
| `main/Kconfig.projbuild` | `CONFIG_XIAOZHI_ENABLE_EYE_ANIM` |

---

## 9. 排障

- **眼睛不显示**：确认已烧 `generated_assets.bin`；串口无 `Eye asset missing` / `Assets partition not ready`。
- **只有左眼或花屏**：查右眼 PSRAM 分配失败日志 `eye mirror alloc failed`，以及是否漏了 `esp_cache_msync`。
- **切表情重启**：确认 BOOT 走 `RequestCycleNext`，`LoadClip` 在 LVGL 锁内。
- **语音切不了**：云端需能调 `self.eyes.*`；本机可用 BOOT 双击或 MCP 验证固件侧。
- **编译去掉眼睛**：`CONFIG_XIAOZHI_ENABLE_EYE_ANIM=n` 后全量编译并烧录。

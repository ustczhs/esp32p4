# ESP32-P4 xiaozhi 会话交接（Handoff）

日期：2026-11-19
工作区：`/Users/tcl/code/esp32/p4`（根目录非 git 仓库；`xiaozhi-esp32/` 是 git 仓库，含用户本地修改，务必保留）

## 任务背景

用户在 Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 板上跑 xiaozhi AI 语音助手固件（用户自己的 fork，含本地人脸追踪、双眼 GIF 动画等改动）。本会话按用户要求做了三轮修改，均已编译、烧录并验证启动日志。

## 环境（详见项目记忆，勿重复搭建）

- 项目记忆：`/Users/tcl/.qoder/projects/-Users-tcl-code-esp32-p4/memory/esp32p4-env-and-board.md`（板卡、双 IDF 布局、依赖 pin 等权威记录）
- xiaozhi 必须用 **IDF v6.1**：`source ~/esp/esp-idf-v6.1/export.sh`（`get_idf` 别名只激活 v5.5.6，勿用）
- 编译：`cd xiaozhi-esp32 && idf.py build`（build 目录已配置好板型，增量编译很快）
- 烧录：`idf.py -p /dev/cu.usbmodem5C941020181 flash`（口号重插可能变，先 `ls /dev/cu.usbmodem*`）
- 抓启动日志：先用 `python -m esptool --chip esp32p4 -p <PORT> --before default-reset --after hard-reset flash-id` 复位，再用 IDF 环境里的 pyserial 读 115200 串口（本会话用过的脚本模式见对话；macOS 无 timeout 命令）
- 机器无 Homebrew/sudo、无 clang-format（esp-clangd 里只有 clangd）；改动按周边代码风格手写即可
- 仓库规范见 `xiaozhi-esp32/AGENTS.md`（勿手改 sdkconfig*/build/managed_components 等生成物）

## 本会话已完成的修改（全部已烧录验证）

1. **WiFi 切换为 "Xiaomi 13"**（密码已写入源码，此处脱敏不记录；见 `main/local_wifi.h`，该文件被 gitignore）
   - `main/boards/common/wifi_board.cc` `TryWifiConnect()`（约 L105）：原逻辑只在 NVS SSID 列表为空时写入本地 WiFi；现改为总是 AddSsid（刷新密码），若该 SSID 是新增则 SetDefaultSsid 置顶。原因：设备 NVS 已存旧 WiFi "TCL-WiFi6"。
2. **摄像头屏幕预览默认关闭**：`main/face_tracker.cc` L134 `settings.GetBool("preview", false)`（原默认 true）。随后第 3 项把整个人脸追踪屏蔽，此项实际已被覆盖。
3. **人脸追踪整体屏蔽**（用户要求注释源码即可）：`main/application.cc` 约 L380，`FaceTracker::GetInstance().Start();` 已注释。检测循环、顶部 `face (nan,nan)` 通知、左下角 debug preview 全部不再出现。VideoStreamer 自行启动 CameraHub，视频流不受影响。
4. **底部字幕条 1 行 → 3 行**：`main/display/lcd_display.cc` 非 wechat 风格 `SetupUI()` 的 `#else`（非 `CONFIG_USE_MULTILINE_CHAT_MESSAGE`）分支，约 L1022：bottom_bar_ 高度 = 3×行高+spacing(8)，label 固定 3 行高、`LV_LABEL_LONG_WRAP`（超出裁剪），删除了原单行循环滚动动画代码。
5. 文档同步：`docs/voice-and-face-track_zh.md` L143 preview 默认值描述已更新（注意：该文档其余部分仍描述人脸追踪开启时的行为，与第 3 项现状不完全一致，未全面改写）。

验证结果（2026-11-19 启动日志）：连上 Xiaomi 13（2.4GHz ch1），IP 192.168.18.208，MQTT 已连接，FaceTracker 输出 0 条，无报错。

## 当前状态与待办

- 固件为最新修改后的版本，已在板上运行。
- **待用户目视确认**：3 行字幕效果（需对话触发显示）；如不合预期需调字号/对齐/裁剪。
- 所有修改都在 xiaozhi-esp32 worktree 中**未提交**（用户有自己的提交习惯，未经要求不要 commit）。`git status` 可见大量用户既有改动，勿动无关文件。
- 旧构建缓存备份 `~/esp/xiaozhi-stale-build-cache`（1.1GB）现已可删，但删除前须征得用户同意。
- 注意事项：板载 WiFi 仅 2.4GHz（ESP32-C6），手机热点需开 2.4GHz/兼容模式；NVS 里 `face/preview` 若曾被 MCP 置 true 会覆盖默认值（人脸追踪现已整体屏蔽，此点暂不相关）。

## 恢复人脸追踪的方法（如用户反悔）

取消 `application.cc` 中 Start() 的注释即可；preview 默认值在 `face_tracker.cc` L134；MCP 工具 `self.face_track.set_enabled` / `set_preview` 在 Start() 内注册。

## Suggested skills

- `diagnosing-bugs`：若后续出现启动失败、WiFi 断连、屏幕显示异常等需要系统化定位的问题。
- `code-review`：若用户要求审查本地 fork 相对上游的改动（worktree 未提交 diff 较大）。
- `research`：若需查 ESP-IDF v6.1 / LVGL9 / esp_video 官方文档细节（如需确认 label 裁剪行为）。
- 一般编译烧录迭代无需调用 skill，直接按上面环境命令执行。
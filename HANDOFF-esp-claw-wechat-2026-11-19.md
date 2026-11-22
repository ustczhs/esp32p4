# ESP32-P4 × esp-claw × 个人微信 实施交接（Handoff）

日期：2026-11-19
工作区：`/Users/tcl/code/esp32/p4`（根目录非 git 仓库；`xiaozhi-esp32/` 是 git 仓库，含用户本地修改，务必保留）

## 本次会话性质

**实施会话：Phase 1/2 已完成并烧录实测，Phase 3 未开始。** 前序调研见 `HANDOFF.md`（纯调研），
更早的固件修改见 `HANDOFF-firmware-2026-11-19.md`（3 行字幕/人脸追踪屏蔽/WiFi 切换，均已烧录）。

**权威文档**：
- 计划与实测记录：`PLAN-esp-claw-wechat-2026-11-19.md`（§11 Phase 1 实测、§12 Phase 2 实测，含全部踩坑）
- 项目记忆：`/Users/tcl/.qoder/projects/-Users-tcl-code-esp32-p4/memory/esp-claw-wechat-integration.md`
- 环境/板卡：同目录 `esp32p4-env-and-board.md`

## 当前设备状态（2026-11-19 实测）

板子上运行 **CLAW_ENABLE=y 固件**（3.77MB，OTA 槽 7MB）：
- xiaozhi 原功能全部正常：WiFi "Xiaomi 13"、MQTT 激活→idle、唤醒词"你好小智"、音频/视频流/字幕，40s+ 无崩溃，free SRAM ≈37.8KB 稳定
- claw 子系统启动成功：FATFS 挂载 `/claw`（首次自动格式化）、2 条种子路由规则加载、root agent id=0 worker 运行、event router 运行，日志 `esp-claw subsystem started`
- **微信未登录、LLM key 未配置**（仅告警不阻塞）→ 微信 agent 功能尚不可用，等 Phase 3
- CLAW_ENABLE=n 零影响已复验：独立 build 目录 198 组件（与移植前一致），claw 组件/源码/依赖全 0

## 环境与命令（勿重复搭建）

```bash
source ~/esp/esp-idf-v6.1/export.sh          # 必须 v6.1（get_idf 别名是 v5.5.6，勿用）
cd /Users/tcl/code/esp32/p4/xiaozhi-esp32

# 启用 claw 编译（sdkconfig 已含 CLAW_ENABLE=y 后，普通 idf.py build 即可）
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32p4;claw.defaults" build

idf.py -p /dev/cu.usbmodem5C941020181 flash  # 口号重插可能变，先 ls /dev/cu.usbmodem*

# 抓启动日志：先复位再读串口（IDF python 环境有 pyserial；macOS 无 timeout 命令）
~/.espressif/python_env/idf6.1_py3.12_env/bin/python -m esptool --chip esp32p4 \
  -p /dev/cu.usbmodem5C941020181 --before default-reset --after hard-reset flash-id
# 然后 pyserial 115200 读 40s（脚本模式见本会话对话；注意复位后立刻开读，否则错过启动窗口）
```

- 机器无 Homebrew/sudo；python 用 IDF 环境绝对路径（shell 里无裸 `python`）
- 组件仓库 CDN 偶发抖动（components-file.espressif.com → .com.cn  redirect 超时）：
  重试即可，必要时 `IDF_COMPONENT_API_TIMEOUT=120 idf.py reconfigure`
- **managed_components 是项目级共享**：禁用版构建会清掉仅 claw 需要的 `espressif__esp_websocket_client`，
  下次启用 configure 自动重新下载（需联网）。本会话结束时正在后台恢复该依赖。

## 代码改动清单（全部在 xiaozhi-esp32/，未 commit——用户有自己的提交习惯）

| 文件 | 改动 |
|---|---|
| `components/esp_claw/` | 新增：从 `/Users/tcl/code/esp32/p4/esp-claw-ref` rsync 移植（排除 test_apps），11 个组件：claw_modules/{claw_utils,claw_endpoint,claw_http,claw_core,claw_cap,claw_manager,claw_event_router,claw_skill}、claw_capabilities/{cap_im_platform,cap_system}、common/http_reuse |
| `CMakeLists.txt`（顶层） | CLAW_ENABLE 门控 EXTRA_COMPONENT_DIRS；**读本次构建的 SDKCONFIG 路径**（非固定根路径），sdkconfig 不存在时回退扫 SDKCONFIG_DEFAULTS；`include(project.cmake)` 后 `idf_build_set_property(CLAW_BUILD_ENABLED ...)` |
| `main/CMakeLists.txt` | `idf_build_get_property(CLAW_BUILD_ENABLED)` 门控 claw 源码/INCLUDE_DIRS/PRIV_REQUIRES（claw_core claw_cap claw_manager claw_event_router cap_im_platform cap_system） |
| `main/Kconfig.projbuild` | 末尾内联 menu "ESP-Claw WeChat Agent"：CLAW_ENABLE(default n)、CLAW_IM_WECHAT、APP_CLAW_CAP_IM_WECHAT(隐藏)、CLAW_LLM_BACKEND/MODEL/BASE_URL。**不要用 Kconfig source 引外部文件**（此 IDF 布局下 $PROJECT_DIR/srctree 均失效） |
| `partitions/v2/32m_face.csv` | 末尾追加 `claw_data, data, fat, , 0x1E0000`（1.875MB，恰好填满 32MB，原偏移未动） |
| `claw.defaults` | 新增：CONFIG_CLAW_ENABLE=y + CONFIG_CLAW_IM_WECHAT=y |
| `main/claw/claw_port.{h,cc}` | 新增：装配任务（mount→paths→种子规则→router_init→**claw_cap_init**→注册 groups→outbound binding→SetupWechat→**claw_cap_start_all**→StartAgent→router_start→SNTP），每步带 `failed at <step>` 定位日志 |
| `main/claw/xiaozhi_bridge.{h,cc}` | 新增：Phase 2 仅观察版 outbound resolver（打日志、返回 ESP_ERR_NOT_FOUND 回退默认微信出站）。**Phase 3 在此实现路线 A 桥** |
| `main/application.cc` | 两处 `#if CONFIG_CLAW_ENABLE` 守护：include claw_port.h；HandleActivationDoneEvent 里 VideoStreamer::Start() 后 ClawPortStart() |

用户本地改动（双眼 GIF/3 行字幕/WiFi/人脸追踪屏蔽）全部保留，未触碰无关文件。

## 关键踩坑（已修，勿重蹈）

1. **IDF early-expansion 无 CONFIG_ 变量**：依赖收集跑在独立 `cmake -P` 进程
   （`tools/cmake/scripts/component_get_requirements.cmake`），`if(CONFIG_X)` 守护的
   PRIV_REQUIRES 会被静默丢弃（源码却正常加入→只报头文件找不到）。
   解法：顶层算开关→`idf_build_set_property`→main 用 `idf_build_get_property`（两阶段均可读）。
2. **多 build 目录时 sdkconfig 路径**：main/顶层必须用 `SDKCONFIG` 变量指向的文件，
   不能硬编码项目根 sdkconfig（否则禁用版误读启用版配置）。sdkconfig 存在时以其为准，
   defaults 仅 clean build 兜底（避免 "sdkconfig=n 但 defaults=y" 的不一致）。
3. **C++ 复合字面量取地址非法**：`&(struct_t){...}` 改命名局部变量（4 处）。
4. **`claw_cap_init()` 必须先于 register_group**（否则 ESP_ERR_INVALID_STATE）；
   启动用 `claw_cap_start_all()`（参考 app_capabilities.c:911/978）。
5. 改顶层 CMakeLists 后若没触发 reconfigure，`idf.py reconfigure` 强制重配。
6. 官方 xiaozhi.me 云 **detect 文本 >≈16 汉字即拒**（Alert: Detect is only for wake words...）；
   `SendWakeWordDetected` 是裸字符串拼接**无 JSON 转义**，桥注入时必须自行转义。

## Phase 1 结论（路线 A 已定）

- 全文经云播报不可行；**路线 A 混合（零新增 TTS key）**：
  ≤16 字摘要经 listen-detect 注入→云端 LLM 播上下文提醒；agent 全文用
  `display->SetChatMessage` 直接上屏（不走云 sentence_start）。
- 注入状态路径：保持 idle→（必要时开音频通道）→`listening_mode_=kListeningModeManualStop`→
  SendWakeWordDetected→tts start 触发 idle→speaking（合法）→tts stop 回 idle；全程不开麦。
- 忙时（listening/speaking/notifying）入小队列（上限 3，超限丢最旧），回 idle 再播；回微信不受设备状态影响。

## Phase 3 待做（下次会话主线）

1. **微信登录 CLI**：`wechat login/status/logout`，二维码 URL 打到串口（qr_data_url[256]），
   NVS 持久化 + 重启恢复。**必须与用户 EyePlayer 的 eye_cli 任务共存**（它也读 stdin；
   spike 曾用 uxTaskGetSystemState 找 "eye_cli" 挂起——正式方案应共存而非挂起）。
   API：`cap_im_wechat_qr_login_start(account_id, force)` / `qr_login_get_status` /
   `qr_login_mark_persisted` / `send_text(chat_id, text)`（见 cap_im_wechat.h）。
2. **入站闭环**：微信消息→router 规则→agent→out_message→回微信（种子规则已就位，
   outbound binding "wechat"→"wechat_send_message" 已注册）。
3. **路线 A 桥**（xiaozhi_bridge.cc）：outbound resolver 里做 ≤16 字摘要 detect 注入
   （idle 门控 + 忙队列 3 条 + JSON 转义）+ 全文屏幕字幕 + TTS 全文累积。
   状态变更走 SetDeviceState，主循环变更走 Schedule。

**用户侧前置（Phase 0 遗留，开工前必须确认）**：
- 个人微信 iLink ClawBot 扫码验证（腾讯官方协议，支持私信+媒体，不支持群聊）
- LLM API key + 服务商/模型名（backend ID 用 "openai_compatible"/"anthropic_compatible"，
  注意不是 "anthropic"；"openai_responses"/"custom" 亦可）

## NVS 键（namespace "claw"，敏感信息只进 NVS 不进源码/文档）

`wechat_token` / `wechat_base_url` / `wechat_account_id` / `wechat_cdn_base_url` /
`llm_api_key` / `llm_model` / `llm_base_url`

## 纪律（沿用 PLAN §9 / AGENTS.md）

不 commit；不碰无关文件；保留全部用户本地改动；不手改 sdkconfig*/build/managed_components
等生成物；CLAW_ENABLE 宏守护所有侵入点；敏感信息（微信 token/LLM key/WiFi 密码）不写入源码/文档；
每阶段结束向用户汇报实测结果再进入下一阶段。
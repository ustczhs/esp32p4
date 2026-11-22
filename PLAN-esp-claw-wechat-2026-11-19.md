# esp-claw × xiaozhi 微信融合方案

日期：2026-11-19
状态：**Phase 1/2 完成（路线 A：短摘要 detect 注入 + 屏幕全文字幕，零新增 key）；Phase 3 待开始（需用户先完成微信 ClawBot 验证 + 提供 LLM key）**
工作区：`/Users/tcl/code/esp32/p4`

## 1. 目标与已确认决策

- 基于 **espressif/esp-claw**（官方端侧 agent 框架）接入**个人微信**（腾讯 iLink ClawBot 通道，扫码登录）。
- **完整保留 xiaozhi 语音对话**：现有音频链、唤醒词、官方 xiaozhi.me 云服务、显示、用户本地改动（双眼 GIF、3 行字幕、WiFi 等）一律不动。
- 微信消息由端上 claw agent 处理，**回复回发微信**，同时**经 xiaozhi 官方云语音播报 + 屏幕字幕**。
- 语音输出路线（用户已选）：**走 xiaozhi 官方云**——把文本注入 xiaozhi 对话（`listen detect`），由小智云端 LLM 回应并 TTS。零新增 TTS key；接受云端可能改写措辞（非逐字朗读）。
- 兜底：若官方云不接受 detect 文本注入（需实测），回退"第三方 TTS（OpenAI 兼容 opus / 火山 ogg_opus）→ 现成 NotifyPlayer 播报"路线（见 §8）。

## 2. 参考源码与已核实事实

本地参考（只读，勿混入 worktree）：
- `esp-claw-ref/` — esp-claw master 源码（codeload tarball，2026-11-19；GitHub 直连当时不通）
- `xiaoclaw-ref/` — 上一轮调研的 xiaoclaw（已放弃该路线，仅留档）

### esp-claw 侧（file:line 相对 esp-claw-ref/）

| 事实 | 证据 |
|---|---|
| 微信 = 腾讯官方 iLink 个人微信 Bot：`get_bot_qrcode?bot_type=3` 扫码 → 轮询 `get_qrcode_status`（wait/scanned/redirect/confirmed）→ confirmed 返回 bot_token/ilink_bot_id/baseurl | components/claw_capabilities/cap_im_platform/src/cap_im_wechat.c:2577,2632,2686-2706 |
| 收消息：长轮询 `ilink/bot/getupdates`（默认 35s），文本 type=1，媒体存 inbox；**支持群聊**（chat_id=group_id） | cap_im_wechat.c:1551,1469,1503-1513 |
| 发消息：`ilink/bot/sendmessage`，4000 字节分片，Bearer+ilink_bot_token；token 仅存内存，持久化由上层负责 | cap_im_wechat.c:1666,1714-1722,630-635 |
| 二维码 TTL 5min 自动刷新 3 次；token 失效不自动重扫码；原版二维码只走 Web 配置页 | cap_im_wechat.c:65-66；http_server_wechat_api.c:59 |
| 出站全局钩子：`router_config.outbound_resolver` 可截获最终回复文本+channel+chat_id；core 完成后发 `out_message` 事件 | claw_event_router.h:21-26,41；router.c:1676-1688；claw_core_events.c:126-133 |
| 出站绑定：`claw_event_router_register_outbound_binding("wechat","wechat_send_message")` | common/app_claw/app_claw.c:915 |
| session 映射 `chat:wechat:<chat_id>`；root agent 由 `claw_agent_mgr_create_root_agent` 直建 | claw_session_mgr.c:226；claw_agent_mgr.h:21,77 |
| LLM 4 后端（openai_compatible/openai_responses/anthropic/custom），`claw_core_config_t` 运行时传 key/model/base_url，支持热更新 | claw_core.h:142-177；claw_core/src/llm/backends/ |
| 最小核心链：claw_utils→claw_endpoint→claw_http→claw_core→claw_skill→claw_cap→claw_manager→claw_event_router→cap_im_platform；**claw_memory 可不装**（providers 可空）；lua/mcp/scheduler/web_search 等全可裁 | 各 CMakeLists REQUIRES；claw_core.h:160 |
| 外部依赖仅 espressif/cjson ^1.7 + esp_websocket_client ^1.4.0；**所有 claw 组件无 IDF 版本 pin**；核心层无旧驱动 API | claw_core/idf_component.yml；cap_im_platform/idf_component.yml |
| app_claw 硬耦合板级管理/audio_hub（会抢 I2S）→ **不移植 app_claw，自写薄装配**；配置接口是函数指针回调，可剥离 | common/app_claw/CMakeLists.txt:2-33；app_claw.c:449-463；app_claw.h:70-75 |
| cap_im_platform 源码由 `CONFIG_APP_CLAW_CAP_IM_*` 门控，组件本身无 Kconfig → 移植需自带开关 | cap_im_platform/CMakeLists.txt；cap_im_platform.c:8-35 |
| agent 任务 8KB 栈/prio5/无核亲和，栈默认 PSRAM；TLS buffer 2048+crt_bundle；TLS 校时需墙钟（SNTP） | claw_core_internal.h:28-30；claw_task.c:68-99；app_claw.c:967 |
| 数据根可配：`claw_paths_set(CLAW_PATH_DATA,...)`；router_rules/sessions/inbox 在 DATA 根，缺省从 `.recovery` 种子复制 | claw_paths.c:21；edge_agent/main/app_fs.c:24,33 |

### xiaozhi fork 侧（file:line 相对 xiaozhi-esp32/）

| 事实 | 证据 |
|---|---|
| 文本注入：`Protocol::SendWakeWordDetected` 发 `{"type":"listen","state":"detect","text":...}`；`SendText` 纯虚，WS/MQTT 双实现 | main/protocols/protocol.cc:75-80；protocol.h:85；websocket_protocol.cc:56；mqtt_protocol.cc:192 |
| TTS 逐句文本在 `sentence_start` 处理并上屏，**无全文累积**（回发微信需自行拼接 start→stop 之间各句） | main/application.cc:651,656,666-679 |
| 现成播报通道：`Application::StartNotification(audio_url, subtitles)` 本地可调，**仅 idle 且播放器空闲才播，忙时丢弃**；Notifying 状态播完回 idle；NotifyPlayer 只收 OGG/Opus（上游 PR #2191） | application.cc:1137-1201；main/notify/notify_player.cc:187 |
| 音频注入备用接口：`AudioService::PushPacketToDecodeQueue` | main/audio/audio_service.cc:609；audio_service.h:144 |
| MCP 新增工具顺手：`AddTool(name,desc,PropertyList,ToolCallback)`，服务端 LLM→mcp 下行→DoToolCall | main/mcp_server.h:434-435；mcp_server.cc:455-473,574 |
| 最佳挂载点：网络就绪后 HandleNetworkConnectedEvent 内、VideoStreamer 启动处并列 | application.cc:383（VideoStreamer::Start）；:303,:347 |
| 依赖现状：cJSON 已有、FATFS 已启用（VOLUME_COUNT=2）；**esp_websocket_client 没有**（WS 走 esp-ml307 封装），需新增 | main/CMakeLists.txt:1134；sdkconfig:3027-3031 |
| 分区：现用 `partitions/v2/32m_face.csv`（2×7MB OTA + 16MB assets），**尾部空闲 1.875MB** 可追加 fatfs 分区；app 现 3.76MB/7MB 槽余量足 | sdkconfig:1301；build/xiaozhi.bin |
| 状态机 idle→connecting/listening/speaking 合法，idle 主动发起对话可行（参考 BeginWakeWordInvoke） | device_state_machine.cc:72-80；application.cc:967 |

## 3. 总体架构

```
微信(个人号/群) ⇄ 腾讯 iLink 云 ⇄ [cap_im_wechat 长轮询]
                                        │ 入站 message 事件
                                 claw_event_router ── 规则 ──→ claw_core agent（LLM 工具循环）
                                        │ out_message（outbound_resolver 钩子截获）
                          ┌─────────────┴─────────────┐
                   wechat_send_message          xiaozhi_bridge（新增，~300行）
                   （回复回发微信）                    │ idle 门控 + 忙时小队列
                                          protocol_->SendText(detect 注入)
                                                │ 官方 xiaozhi.me 云
                                          LLM 回应 → tts 音频 + sentence_start 文本
                                                │
                                          设备扬声器播报 + 3行字幕（现有链路，零改动）
```

- xiaozhi 语音对话（唤醒词→ASR→云→TTS）**原样保留**，claw 子系统是并列的旁路。
- 微信入站默认全部交给 agent；agent 回复双路输出：回微信（逐字）+ 设备播报（经云，措辞可能被改写，用户已接受）。
- 播报注入文本带轻前缀（如"微信消息：…"或"我在微信回复了：…"），给云端 LLM 上下文；前缀策略做成可配。
- 设备忙（listening/speaking/notifying）时播报入小队列（上限 3 条，超限丢最旧），回 idle 再播；回微信不受设备状态影响。
- 可选增强（Phase 4）：注册 MCP 工具 `self.wechat.send_message` / `self.wechat.read_recent`，让**语音**也能指挥微信（"给 XX 发微信"），复用 cap_im_wechat 发送 API。

## 4. 移植清单

### 4.1 引入组件（新增 `xiaozhi-esp32/components/esp_claw/`，源码拷贝自 esp-claw-ref，保持上游目录名）

核心（必需）：`claw_utils`、`claw_endpoint`、`claw_http`、`claw_core`、`claw_cap`、`claw_manager`、`claw_event_router`、`claw_skill`（可选但建议，技能系统轻量）
能力（必需）：`cap_im_platform`（Kconfig 只开 WECHAT，其余 QQ/FEISHU/TG=n）、`cap_system`（时间/SNTP；其 esp_wifi 调用 P4 上由 esp_wifi_remote 满足，若编译受阻可裁掉 wifi 部分只留 SNTP）
可选后补：`claw_memory`（长期记忆）、`cap_files`、`cap_scheduler`、`cap_mcp_client`（连主机 OpenClaw 的钩子）、lua 全家桶
**不移植**：`common/app_claw`（抢 I2S、耦合板级管理）、`wifi_manager`（用 xiaozhi 现有网络）、`http_server`/Web 前端、`display_service`/`emote`、`audio_hub`、`cap_im_local`（暂不需要 web 通道）

### 4.2 新增胶水代码（全部在新目录，现有文件最小侵入）

| 模块 | 职责 | 估算 |
|---|---|---|
| `claw_port/`（装配层） | 替代 app_claw：claw_paths_set→router_init→cap 注册→outbound 绑定→root agent→router_start→SNTP；配置回调对接 xiaozhi Settings/NVS；LLM key/model 从 NVS 读 | ~200-300 行 |
| `xiaozhi_bridge/` | outbound_resolver 钩子：截获 wechat 出站文本→Schedule 到主循环→idle 门控+队列→detect 注入；TTS start/stop 间全文累积（供日志/未来回发） | ~300 行 |
| 微信登录 CLI | console 命令 `wechat login`（打印二维码 URL 到串口，手机扫码）/`wechat status`/`wechat logout`；confirmed 后 token/baseurl/bot_id 存 NVS（namespace `claw`），重启自动恢复 | ~150 行 |
| Kconfig | `CLAW_ENABLE`（总开关，default n）、`CLAW_IM_WECHAT`、LLM provider/model/base_url 默认值（key 走 NVS 不进源码） | ~60 行 |

### 4.3 现有文件改动点（最小侵入，全部可被 CLAW_ENABLE 宏包裹）

- `main/application.cc`：:383 附近并列启动 claw 子系统（网络就绪后）；OnIncomingJson 的 tts start/stop 处加全文累积回调钩子（bridge 订阅，不改原逻辑）
- `main/idf_component.yml`：新增 `espressif/esp_websocket_client: ^1.4.0`（注意 registry 漂移，必要时 pin 精确版本，沿用既有 pin 风格）
- `main/CMakeLists.txt`：新增胶水源文件与 components 目录（EXTRA_COMPONENT_DIRS）
- `partitions/v2/32m_face.csv`：**尾部追加** `claw_data, data, fat, , 0x1E0000`（1.875MB，不动任何现有偏移；assets/NVS 数据不受影响）
- `main/Kconfig.projbuild`：source 新增 Kconfig
- 不动：音频链、协议实现、显示、板级、用户全部本地改动

### 4.4 存储与凭据

- FATFS：`esp_vfs_fat_spiflash_mount_rw_wl("/claw", "claw_data", ...)`；claw_paths DATA 根=/claw；种子 router_rules.json（默认规则：wechat message→run_agent；out_message→send_message 回原聊天）随固件打包或首启生成
- NVS（namespace `claw`）：wechat_token/wechat_base_url/wechat_bot_id、llm_api_key/llm_model/llm_base_url
- 密钥一律 NVS/运行时配置，**不写入源码与文档**（沿用 local_wifi.h 的 gitignore 模式可选）

## 5. 分阶段实施

### Phase 0 — 前提验证（不动固件，半天）
1. **iLink 准入实测**：手机微信按 esp-claw 文档流程扫码绑定 ClawBot（可先借 esp-claw 官方在线烧录固件或 Web 配置页验证账号能登录）；确认 getupdates/sendmessage 可用。**这是全方案最大外部风险**——代码无白名单说明，但服务准入策略未知。
2. 准备 LLM API key（anthropic 或 OpenAI 兼容端点，agent 推理用）。
3. 网络确认：设备当前 "Xiaomi 13" 热点可出公网即可（微信/LLM 都是云端 API，无需局域网互通）。

### Phase 1 — detect 注入可行性 spike（最小改动，1 天内）
1. 在 fork 加一个临时 console 命令/MCP 工具：idle 时调 `protocol_->SendText`（listen detect）注入一句测试文本。
2. 实测官方 xiaozhi.me：是否触发 LLM 回应 + TTS 播报 + 字幕；listening/speaking 时注入的行为；MQTT 与 WS 两种协议都试（当前 MQTT 已连）。
3. **通过**→按主路线继续；**不通过**→启用 §8 TTS 兜底路线（bridge 改接 NotifyPlayer，其余架构不变）。
4. 验证后移除临时代码。

### Phase 2 — esp-claw 核心移植 + 编译（2-4 天，主要不确定性在 IDF 6.1）
1. 拷贝 §4.1 组件到 `components/esp_claw/`，自带 Kconfig 开关，CLAW_ENABLE=n 时零编译。
2. 写 claw_port 装配层；新增 esp_websocket_client 依赖；追加 fatfs 分区。
3. `source ~/esp/esp-idf-v6.1/export.sh && idf.py build`；逐个解决组件兼容问题（预期问题面小：核心无 IDF pin、无旧驱动 API；风险点 esp_websocket_client 版本解析、cap_system 的 esp_wifi 引用）。
4. 里程碑：CLAW_ENABLE=y 编译通过、烧录后 xiaozhi 原功能回归正常（语音对话/字幕/WiFi），claw 子系统启动日志正常、FATFS 挂载成功。

### Phase 3 — 微信通道 + 双向融合（2-3 天）
1. 微信登录 CLI：扫码→token 持久化→重启恢复；掉线/token 失效提示重新扫码。
2. 入站：微信文本→agent→回复回微信（先验证这条闭环，含群聊@场景）。
3. 出站播报：outbound_resolver→bridge→detect 注入→设备播报+字幕；忙时队列；TTS 全文累积日志。
4. 里程碑：微信发消息→设备开口播报+字幕→微信收到 agent 回复；语音对话全程不受影响；对话进行中发微信→回复照常回微信、播报排队到空闲。

### Phase 4 — 可选增强（按需）
- MCP 工具 `self.wechat.*`：语音指挥发微信/查最近消息
- claw_memory 长期记忆、cap_scheduler 定时任务（"每天 8 点微信提醒我…"）
- 微信图片入站→cap_llm_inspect 图像理解（需视觉模型）
- cap_mcp_client→局域网主机 OpenClaw（更复杂任务）
- 飞书通道并行启用（cap_im_feishu 现成，Kconfig 一开即可，作为 iLink 不可用时的替代）

## 6. 资源与共存核算

- Flash：app 3.76MB + claw 核心/微信（估 +0.5-1MB）≪ 7MB OTA 槽；claw_data 1.875MB 存 sessions/rules/inbox 足够（不装 lua/技能包时）
- RAM：agent 8KB 栈 prio5 无亲和（栈 PSRAM）、微信轮询任务、TLS buffer 2KB/连接；大缓冲走 PSRAM（32MB 充裕）；与 xiaozhi AFE（核0 prio8）无优先级冲突
- 网络：微信长轮询 35s + LLM HTTPS + xiaozhi MQTT/WS 并存，esp_hosted(C6) 带宽足够（文本为主）
- 音频：**claw 不碰 I2S/codec**（app_claw/audio_hub 已排除）；播报完全走 xiaozhi 现有 TTS 链路
- 主循环：bridge 所有对 Application 的调用经 `Schedule()`（遵守 fork AGENTS.md：回调不在主任务、不阻塞音频路径）

## 7. 风险与对策

| 风险 | 等级 | 对策 |
|---|---|---|
| iLink 服务准入/扫码绑定失败 | **高**（外部依赖） | Phase 0 先验证；失败→飞书通道兜底（cap_im_feishu 现成，架构不变） |
| 官方云不接受 detect 文本注入 | 中 | Phase 1 先 spike；失败→TTS 兜底（§8），架构不变 |
| IDF 6.1 组件兼容（官方仅验证 5.5.x） | 中低 | 核心 API 面干净无 pin；沿用既有"最小 targeted pin"策略逐个解决 |
| 云端播报改写 agent 回复措辞 | 已接受 | 用户已确认；前缀策略可调；关键任务结果同时逐字回微信 |
| 微信 token 失效不自动重扫码 | 低 | CLI 提示 + 设备字幕/Alert 提醒重新登录 |
| 忙时播报丢失 | 低 | bridge 小队列（上限 3）；现有 notify 的"忙即丢"策略不复用 |
| registry 漂移（esp_websocket_client 解析） | 低 | 精确 pin，沿用 idf_component.yml 既有修复风格 |
| 分区表变更 | 低 | 仅尾部追加，现有偏移不动；烧录后验证 assets/NVS 完好 |

## 8. 兜底路线（仅当 Phase 1 detect 注入失败）

bridge 输出改接现成 `Application::StartNotification`（application.cc:1137）：
- agent 回复文本 → 第三方 TTS HTTP API（OpenAI 兼容 `/v1/audio/speech` response_format=opus，或火山引擎 ogg_opus）→ 返回 OGG/Opus 流 URL → StartNotification 播报+字幕（NotifyPlayer 原生支持，notify_player.cc:187）
- 需新增一个 TTS API key（可与 LLM 同商）；若 TTS 只返回 MP3，需评估 NotifyPlayer 扩展或主机转码（尽量避免）
- 逐字播报、不经过云端 LLM，其余架构（微信闭环、队列、门控）完全不变

## 9. 执行纪律（沿用 handoff 约定）

- 不 commit（用户有自己的提交习惯）；不碰无关文件；保留全部本地改动（双眼 GIF/3 行字幕/WiFi/人脸追踪屏蔽）
- 遵守 xiaozhi-esp32/AGENTS.md：不手改 sdkconfig*/build/managed_components 等生成物；状态变更走 SetDeviceState；主循环变更走 Schedule；CLAW_ENABLE 宏守护所有侵入点
- 编译烧录按 HANDOFF-firmware-2026-11-19.md：IDF v6.1（`source ~/esp/esp-idf-v6.1/export.sh`），端口先 `ls /dev/cu.usbmodem*`
- 敏感信息（微信 token、LLM key、WiFi 密码）不写入源码/文档
- 每阶段结束向用户汇报实测结果，再进入下一阶段

## 10. 待用户提供

1. 个人微信号配合 Phase 0 扫码验证 iLink ClawBot
2. LLM API key + provider/model（anthropic 或 OpenAI 兼容）
3. 确认本方案后开始 Phase 0/1

## 11. Phase 1 实测结果（2026-11-19，MQTT + 官方 xiaozhi.me 云）

临时 spike：`main/claw_spike.cc` + `Application::SpikeInjectText`（stdin 命令 `inject/injectf/state`；
注意：用户 EyePlayer 的 eye_cli 任务也读 stdin，spike 启动时将其挂起，移除 spike 时恢复）。

1. **长文本注入被官方云明确拒绝**：`Alert ERROR: Detect is only for wake words, do not send long texts.`
2. **长度阈值 ≈16 个汉字**：16 字接受、18 字拒绝（17 未测）。
3. **短文本（≤16字）注入完全可行**：云端把 detect 文本当用户话语 → LLM 上下文回应 → TTS 播报 + 字幕；
   例："微信消息：张三说今天开会" → 设备播"张三说今天开会，你知道是几点吗？"。
4. **状态路径验证**：注入后保持 idle，tts start 触发 `idle→speaking`（合法），tts stop（manual 模式）回 idle；
   全程不开麦、不进 listening，不干扰唤醒词。
5. 被拒后设备无损：仅 Alert，唤醒词对话照常。
6. 结论：**"全文经云播报"主路线不可行**。可选路线：
   - **A 混合（零新增 key）**：detect 注入 ≤16 字摘要（如"微信消息：张三说三点开会"）→ 云端播上下文提醒；
     屏幕字幕用 `display->SetChatMessage` 直接显示 agent 完整回复（不走云 sentence_start）。
     语音是提醒而非逐字回复。
   - **B TTS 兜底（§8）**：第三方 TTS → NotifyPlayer 逐字播报全文 + 字幕；需一个 TTS key。
   - A/B 可并存：默认 A，配置切换 B。
## 12. Phase 2 实测结果（2026-11-19，IDF v6.1.1 / Waveshare P4）

1. **编译**：`CLAW_ENABLE=y` 全量编译通过（2440 目标），固件 3.77MB（OTA 槽 7MB）。
   启用方式：`idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32p4;claw.defaults" build`。
2. **移植踩坑（已修）**：
   - IDF 依赖收集跑在独立 CMake 进程（early expansion），其中 `CONFIG_*` 变量全为空 →
     `if(CONFIG_CLAW_ENABLE)` 守护的 PRIV_REQUIRES 被静默丢弃。改为该阶段直接读 sdkconfig；
     顶层 EXTRA_COMPONENT_DIRS 同时回退扫描 SDKCONFIG_DEFAULTS（支持 clean build）。
   - C++ 不允许对复合字面量取地址 → 4 处改为命名局部变量。
   - 漏掉参考工程的 `claw_cap_init()`（注册 cap group 前必须调用，否则 INVALID_STATE）；
     并按参考用 `claw_cap_start_all()` 统一启动（替代单独 cap_im_wechat_start）。
   - 装配每步带失败定位日志（`failed at <step>`）。
3. **烧录后实测**：FATFS 首次自动格式化并挂载 `/claw`；种子路由规则写入并加载（2 条）；
   claw_cap 初始化、root agent 创建（id=0，worker 已启动）、event router 任务启动，
   日志 `esp-claw subsystem started`。未登录微信/无 LLM key 时仅告警不阻塞启动。
4. **xiaozhi 回归正常**：WiFi "Xiaomi 13" 连接、MQTT 激活完成→idle、唤醒词 wn9_nihaoxiaozhi_tts
   加载、ES7210/BoxAudioCodec 正常、视频流正常；运行 40s+ 无崩溃，free SRAM ≈37.8KB 稳定。
5. 分区：`claw_data`（FAT，1.875MB）追加在 32MB 末尾，原有分区偏移未动，总布局恰好 0x2000000。
6. 待办（Phase 3 前置）：用户侧个人微信 iLink ClawBot 扫码验证；LLM API key/模型/base_url
   （写入 NVS `claw/llm_api_key` 等，不入源码）。
7. **CLAW_ENABLE=n 零影响复验通过**（独立 build 目录 + 独立 sdkconfig）：198 个组件（与移植前一致），
   claw 组件/源码/依赖均为 0，esp_websocket_client 不被引入。
   注意：首轮复验暴露 main 回退读 sdkconfig 必须用本次构建的 `SDKCONFIG` 路径（而非项目根固定路径），
   已改为顶层计算开关后经 `idf_build_set_property(CLAW_BUILD_ENABLED)` 传递，main 用
   `idf_build_get_property` 读取（early-expansion 与正常阶段均可用）；sdkconfig 存在时以其为准，
   SDKCONFIG_DEFAULTS 仅在 clean build 时兜底。
   另：managed_components 为项目级共享，禁用构建会清掉仅 claw 需要的 esp_websocket_client，
   下次启用 configure 自动恢复（IDF 组件管理器正常行为）。

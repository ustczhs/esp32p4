# PLAN: 微信 agent 迁移到 backone 后端（设备仅保留授权 + 消息连接）

日期：2026-11-22
状态：**已实现并实机验证**（唤醒对话、微信聊天 + 语音播报正常；后续又加了 MCP 工具调用与定时提醒）
前置文档：`PLAN-esp-claw-wechat-2026-11-19.md`（端侧 agent 原方案）、backone `HANDOFF.md` / `docs/xiaozhi_protocol.md`

## 1. 已确认的决策

| 问题 | 决策 |
|---|---|
| 后端载体 | 扩展现有 backone（FastAPI），不新建服务 |
| 微信网关轮询位置 | 保留在设备（token 留 NVS，扫码授权流程不变），设备只转发消息 |
| 会话共享 | 微信 agent 与语音助手各自独立会话/上下文 |
| 设备播报 | 要：TTS 播报 + 屏幕字幕，替换失效的 Route A detect 注入 |
| 设备↔后端微信通道 | 独立 WS 连接，与语音会话生命周期解耦 |
| 播报时机 | 设备侧排队等空闲；后续可在后端加 agent 策略 |
| 消息类型 | 第一期仅文本，附件后续再说 |
| 后端不可达 | 设备直接回微信一句固定提示 |

## 2. 目标架构

```
微信 iLink 网关
   ↑↓ cap_im_wechat 轮询/发送（不变：QR 登录、token NVS、wechat CLI）
ESP32
   ├─ claw_event_router（保留，vendor 不改）
   │    规则 v2：入站 text → call_cap "backend_forward"（替换 run_agent）
   ├─ backend_link（新增，main/claw）：独立 WS ↔ backone /wechat/v1/
   │    上行：微信消息 JSON；下行：回复 JSON → cap_im_wechat_send_text + 播报队列
   ├─ 播报 worker（改造 application 的 ClawAnnounce）：空闲时开语音会话发 tts_push
   └─ 删除：claw_core/agent_mgr 初始化、LLM HTTP、llm CLI、Route A 注入桥
   ↕ ws://<backone>/wechat/v1/（控制） + 现有 /xiaozhi/v1/（语音/播报音频）
backone
   ├─ 现有语音会话管线：不动
   └─ 新增 wechat 模块：/wechat/v1/ WS 端点 + 每 chat 独立上下文的 LLM agent
        复用 providers.llm（TCL 端点 doubao-seed-character，key 在 .env）
```

claw_core / claw_agent_mgr 组件因 router 的 CMake REQUIRES 仍会被链接，但**不再初始化**：
无 agent 任务、无端侧 LLM TLS 连接、无工具循环。vendor 代码（components/esp_claw）零改动。

## 3. 通信协议草案

### 3.1 微信控制通道 `/wechat/v1/`（新增，JSON over WS）

握手：复用语音 WS 的鉴权头（`Device-Id` + `Authorization: Bearer <DEVICE_TOKENS>`）。
设备端 URL 从 NVS `ota_url` 推导（`http://H:P/xiaozhi/ota/` → `ws://H:P/wechat/v1/`），
token 复用现有 OTA 流程拿到的值，不新增配置项。

上行（设备 → 后端）：
```json
{"type":"message","chat_id":"...","sender_id":"...","message_id":"...","text":"...","ts":1234567890}
```

下行（后端 → 设备）：
```json
{"type":"reply","chat_id":"...","text":"...","announce":true}
```
- `announce` 默认 true；后端 agent 策略以后可置 false（只回微信不播报）。
- 设备收到 reply：`cap_im_wechat_send_text(chat_id, text)`；若 announce → 入播报队列。
- 断线重连：设备端指数退避（1s 起，上限 30s）；连接期间入站消息排队（上限 8 条，溢出丢最旧并记日志）。
- **不可达提示**：WS 未连接或发送失败时，设备直接回微信固定文案
  「助手暂时不在线，请稍后再试」；同一 chat 60s 冷却，避免刷屏。

### 3.2 播报通道（复用现有语音 WS `/xiaozhi/v1/`）

新增一种上行消息（xiaozhi 协议扩展，backone 文档同步更新）：
```json
{"type":"tts_push","text":"要播报的完整文本"}
```
backone 收到后开一个 **TTS-only 会话**：`tts start` → `sentence_start`（字幕）→
Opus 音频下行 → `tts stop` → `goodbye` 关连接。设备端完全走现有
tts start/stop → speaking 状态机和音频解码路径，**不再有 detect 长度限制、
不再产生 stray 唤醒会话**。每次播报都是新建 channel（fresh hello），
规避旧 NAT-UDP 映射问题。

设备播报 worker：
- 队列上限 3，满则丢最旧；仅在设备 idle 时取队首 → OpenAudioChannel → 发 tts_push。
- 设备正在 listening/speaking（语音对话中）→ 等待，对话结束回 idle 再播。
- 字幕由现有 `sentence_start` 显示路径承担；情绪动画第一期不做（后续可让后端带 emotion）。

## 4. 设备端改动清单（全部 CLAW_ENABLE 守卫，vendor 不动）

| 文件 | 改动 |
|---|---|
| `main/claw/claw_port.cc` | 删 `StartAgent()`、`kSystemPrompt`、llm NVS 读取；router 配置 `default_route_messages_to_agent=false`、`outbound_resolver=nullptr`；种子规则换 v2（入站 text → `call_cap backend_forward`，删 run_agent / agent_out 两条）；注册自定义 cap group；**规则迁移**：检测现有 rules 文件无 v2 规则则覆写（旧设备 FATFS 里已有 v1）；附件 `enable_inbound_attachments=false`（仅文本） |
| `main/claw/backend_link.{h,cc}`（新增） | 复用 `78__esp-ml307` 的 `WebSocket` 类；连接/重连/收发 JSON；入站消息队列；reply → `cap_im_wechat_send_text` + `ClawAnnounce`；不可达固定回复 + 冷却 |
| `main/claw/backend_cap.{h,cc}`（新增，或并入 backend_link） | `claw_cap_register_group` 注册 `backend_forward`：execute 解析 input_json（`{{event.chat_id}}` 等模板）→ 入队，**不阻塞 router 任务** |
| `main/claw/xiaozhi_bridge.{h,cc}` | 删除（Route A 桥整体移除） |
| `main/application.{h,cc}` | `ClawAnnounce(summary, full)` / `ClawInjectDetect` 及 busy 队列 drain 逻辑删除；新 `ClawAnnounce(text)` = 入播报队列；idle 事件驱动 tts_push worker（Schedule 到主任务，遵守状态机） |
| `main/claw/wechat_cli.cc` | 删 `llm key/model/url/status` 子命令；保留 `wechat login/status/cancel/logout`。NVS 中 llm_* 旧键留存不读（无害，不做擦除） |
| `main/protocols/protocol.*` 或 websocket_protocol | 新增 `SendTtsPush(text)`（仅 WS 传输实现；MQTT 路径返回不支持——本项目只用 WS） |
| `claw.defaults` | 不变（CLAW_ENABLE、MEMPOOL_PREFER_SPIRAM 保留） |

不动：`cap_im_wechat` 全部、QR 登录 watcher、wechat NVS 键、eye_cli、ota url CLI、
语音管线、所有本地 UI/WiFi 定制。

## 5. backone 改动清单

| 位置 | 改动 |
|---|---|
| `voice_backend/wechat/`（新增包） | `agent.py`：每 chat_id 独立历史（deque，约 20 轮上限），微信专用 system prompt（简体中文、简短口语化、无 Markdown、无 [END]/[emotion] 标记），调 `providers.llm`；每 chat 串行处理，跨 chat 并发 |
| | `link.py`：`/wechat/v1/` WS 端点处理——鉴权同 DEVICE_TOKENS；message → agent → reply 下行；announce 默认 true |
| `voice_backend/app.py` | 挂载 wechat WS 路由（与语音路由并存） |
| `voice_backend/xiaozhi/adapter.py` | 识别 `tts_push`：跳过 ASR/turn，直接 TTS 合成流式下发，播完 goodbye 关连接 |
| `voice_backend/config.py` | `WECHAT_AGENT_ENABLED`（默认 true）、`WECHAT_SYSTEM_PROMPT`、历史上限等 |
| `docs/xiaozhi_protocol.md` | 新增 §：/wechat/v1/ 控制通道 + tts_push 消息 |
| `tests/` | wechat agent 单测（fake provider）；WS 端点集成测试（TestClient）；tts_push 会话测试；不可达/断线行为 |

LLM key 只存 backone `.env`（已有），设备 NVS 里的旧 key 不再使用。

## 6. 分阶段实施与验证（每阶段汇报实测结果后再进下一阶段）

**P1 backone wechat 模块**（纯后端，不动固件）
- agent + link + 测试全绿；用脚本模拟设备 WS 客户端验证 message→reply。

**P2 固件瘦身 + 控制通道**（播报先关闭：reply 一律 announce=false 或 worker 暂不启用）
- 先做小 spike 验证两点：router 不初始化 agent 时 init/start 正常；
  `call_cap` 动作的 `{{event.*}}` 模板注入可用（v1 规则已在 send_message 上验证过模板）。
- CLAW_ENABLE=y 与 =n 双构建通过；烧录后实测：
  重启恢复登录 → 微信发消息 → backone 日志收到 → 微信收到 LLM 回复；
  停掉 backone → 微信收到不可达提示（含冷却）；语音助手回归不受影响。

**P3 播报链路**
- backone tts_push + 固件播报 worker；实测：idle 时微信回复**有声音** + 字幕；
  语音对话中到达 → 排队，对话结束补播；连续多条 → 队列行为正确。

**P4 总回归**
- 多轮语音（唤醒、barge-in、goodbye、情绪）；微信并发（idle/listening 两态）；
  长时间空闲后 NAT 过期场景；free_sram 观察；CLAW_ENABLE=n 零影响复验。

## 7. 风险与待定项

1. router 无 agent 时的行为需 spike 确认（P2 第一步）；若 init 强依赖 agent，
   备选：保留一个最小 agent 初始化但不路由任何消息给它（仍无 LLM 流量）。
2. tts_push 会话的音频可听性理论上成立（fresh channel + backone 已验证语音可听），
   但必须 P3 实机确认——这是旧 Route A 的遗留疑点。
3. 微信控制 WS 的 token 获取时机（OTA 之后）与重连时序，P2 实现时定稿。
4. 设备端 `SendTtsPush` 属于 Protocol 契约扩展：MQTT 传输不实现（返回 false），
   符合"仅 WS"的现状；如未来要支持 MQTT 再补。
5. 播报队列丢弃策略（丢最旧）如体验不佳，改后端策略（announce=false 或摘要）。

## 8. 纪律

- 本方案确认前不改任何代码；每阶段实测汇报后再继续。
- 不 commit（用户自有提交习惯）；secrets 只进 .env/NVS，不落文档。
- vendor（components/esp_claw、managed_components）零改动。
- 所有固件侵入点保持 CLAW_ENABLE 守卫，=n 构建零影响。
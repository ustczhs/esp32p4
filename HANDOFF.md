# ESP32-P4 × Claw 融合方案调研交接（Handoff）

日期：2026-11-19
工作区：`/Users/tcl/code/esp32/p4`（根目录非 git 仓库；`xiaozhi-esp32/` 是 git 仓库，含用户本地修改，务必保留）

## 本次会话性质

**纯调研，未写任何代码、未编译、未烧录、未改动 xiaozhi-esp32 worktree。** 产出是可行性结论与架构选型（见下）。上一会话的固件修改记录已改名保留为 `HANDOFF-firmware-2026-11-19.md`（3 行字幕、人脸追踪屏蔽、WiFi 切换等，均已烧录验证）。

## 环境与既有状态（勿重复搭建，详见权威记录）

- 项目记忆：`/Users/tcl/.qoder/projects/-Users-tcl-code-esp32-p4/memory/esp32p4-env-and-board.md`（板卡 Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3、32MB Flash + 32MB PSRAM、双 IDF 布局、依赖 pin）
- 上一会话交接：`HANDOFF-firmware-2026-11-19.md`（编译/烧录/抓日志命令、待办）
- 要点：xiaozhi 用 IDF v6.1（`source ~/esp/esp-idf-v6.1/export.sh`）；板载 WiFi 走 ESP32-C6 协处理器（esp_hosted），仅 2.4GHz；当前连 "Xiaomi 13" 热点；机器无 Homebrew/sudo，工具用户级安装（Node 24 已有，npm 可用）
- 遗留待办（沿用上一会话）：用户目视确认 3 行字幕效果；`~/esp/xiaozhi-stale-build-cache`（1.1GB）删除前须征得同意

## 用户目标

xiaozhi 负责语音交互；"claw 类工具"负责复杂推理 + **个人微信**接入。用户第二轮明确倾向：把轻量 claw 直接装进 ESP32（不一定 OpenClaw），与 xiaozhi 音频融合。第三轮用户给出参考项目 beancookie/xiaoclaw，问可否借鉴。

## 调研结论（三轮，按序）

### 第 1 轮：OpenClaw 在主机侧（方案 A，仍有效作兜底）

- 架构：ESP32（xiaozhi 固件只改 OTA/WS 地址）→ 自建 xiaozhi-esp32-server（ASR/TTS）→ OpenClaw Gateway 的 OpenAI 兼容端点 `/v1/chat/completions`（默认关闭，`gateway.http.endpoints.chatCompletions.enabled: true`；`model: "openclaw/default"`；`user` 字段或 `x-openclaw-session-key` 保持会话；该端点=完全操作权限，仅限局域网+token）。
- 个人微信：**腾讯官方维护**的 OpenClaw 插件 `@tencent-weixin/openclaw-weixin`，扫码登录个人微信（iLink API），支持私信+媒体，**不支持群聊**；插件 2.x 需 OpenClaw ≥2026.5.12。安装：`npx -y @tencent-weixin/openclaw-weixin-cli install`。
- 备选：官方 xiaozhi.me + MCP 接入点挂 OpenClaw（改动最小但受平台约束）。
- 坑：手机热点下 Mac 与 ESP32 可能客户端隔离不互通；OpenClaw 多步任务延迟大，语音场景需简洁人设或异步"办完微信通知"。

### 第 2 轮：端上轻量 claw 选型

| 项目 | 结论 |
|---|---|
| PicoClaw | Go，需 Linux，❌ 排除 |
| NullClaw | Zig 静态二进制，面向 Linux/POSIX，❌ 跑不了 MCU |
| zclaw (tnm/zclaw) | 纯 C 888KiB 全固件，ESP32-C3，极简，Telegram 类通道 |
| **MimiClaw (memovai/mimiclaw)** | ✅ 最佳底座：纯 C、ESP-IDF v5.5+、S3 目标（16MB Flash+8MB PSRAM）；ReAct 工具循环、FATFS/SPIFFS 记忆文件（SOUL/USER/MEMORY.md）、cron、心跳、WS 网关 18789、OTA；通道仅 **Telegram+飞书，无微信、无音频** |
| TClaw (tuya/TClaw) | 声称 ESP32+微信/QQ/飞书通道，但仓库多次抓取失败**未核实**；微信疑似公众号/企微机器人走涂鸦云，非个人微信 |

- 移植 MimiClaw 到 P4 的工作量：RISC-V 目标适配 + esp_hosted WiFi（可复用用户 fork 的 `wifi_board` 代码）+ IDF 5.5→6.1 对齐。
- **个人微信在端上不可行**（无 C 实现，官方插件是 Node.js）→ 微信腿必须有一台常开主机跑 OpenClaw。
- 推荐混合架构：P4 上 xiaozhi+内嵌 agent；Mac/小主机 OpenClaw+微信插件；两者局域网互通。若用户接受飞书/Telegram 替代微信，可纯端上无主机。

### 第 3 轮：beancookie/xiaoclaw（关键发现，下一步工作围绕它）

- **就是 xiaozhi-esp32（语音 I/O）+ mimiclaw（agent 大脑）+ Bridge 层的现成单固件融合**，MIT，~1009 commits，ESP32-S3 + 32MB Flash + 8MB PSRAM，IDF v5.5+。官网 https://beancookie.github.io/xiaoclaw/ ，V2EX 帖 https://hk.v2ex.com/t/1204485 。
- 三层：xiaozhi 语音层（唤醒词/Opus/显示，ASR/TTS 仍走服务端）；mimiclaw agent 层（ReAct、记忆、会话压缩、cron、自学习技能沉淀、checkpoint 崩溃恢复）；**Bridge 层**（ASR 文本→路由→本地 agent 或原服务端→TTS）。
- **MCP 客户端**（`tool_mcp_client.c`，Kconfig `CONFIG_MIMI_MCP_REMOTE_HOST/PORT/EP`，示例 `scripts/mcp_server.py`）：端上 agent 可连远程 MCP server——**这是接主机 OpenClaw/微信的现成钩子**。
- 分区参考：fatfs ~12MB 存记忆/技能，双 5MB OTA；任务布局：agent_loop 独占核 1（prio 6，24KB 栈），tg_poll/feishu_ws 核 0。
- 差距：仅 S3 目标（P4 需移植）；xiaozhi 底座是旧 fork，**不含用户本地改动**；无微信。
- **已定移植方向（用户未反对）：把 xiaoclaw 的 mimiclaw+bridge+MCP client 作为组件移植进用户的 xiaozhi-esp32 fork**（保留 P4 板级支持、IDF 6.1 适配、双眼 GIF/3 行字幕等全部改动），而不是把用户改动搬进 xiaoclaw。

## 下一步（已与用户对齐方向，待执行）

1. **clone beancookie/xiaoclaw 到本地只读研究**（建议放 `/Users/tcl/code/esp32/p4/xiaoclaw-ref/` 之类，勿混入 xiaozhi-esp32 worktree），重点评估：
   - bridge 路由实现（如何决定走本地 agent 还是原服务端；对 xiaozhi 协议/应用层的侵入点）
   - 它的 xiaozhi 底座版本/快照时间，与用户 fork（~2026-09-30 快照）的差异
   - mimiclaw 部分对 S3/IDF 5.5 的硬依赖清单（PSRAM 配置、esp-sr、分区、console、WiFi 假设）
2. 产出具体移植评估给用户，确认后再动用户 fork（动手前遵守：不 commit、不碰无关文件、保留全部本地改动）。
3. 主机侧（Mac OpenClaw + 微信插件）可并行先行，独立可验证，不动固件。
4. 网络前提验证：ESP32 与 Mac 在当前热点下能否互通（不行则换同一路由 WiFi）。

## 关键链接

- https://github.com/beancookie/xiaoclaw （核心参考）
- https://github.com/memovai/mimiclaw （agent 底座，README_CN + docs/ARCHITECTURE.md + docs/im-integration/）
- https://github.com/xinnan-tech/xiaozhi-esp32-server （自建服务端）
- https://github.com/prestouu/xiaozhi-openclaw-bridge （主机侧桥接先例，含踩坑记录）
- https://docs.openclaw.ai/gateway/openai-http-api （OpenAI 兼容端点）
- https://docs.openclaw.ai/zh-CN/channels/wechat （官方微信插件）
- https://github.com/tnm/zclaw 、https://github.com/tuya/TClaw （备选，后者未核实）

## 注意事项

- 本会话所有结论基于网络调研；xiaoclaw/TClaw 源码均未本地验证，移植评估前不要向用户承诺细节。
- GitHub blob/raw 抓取偶发失败/超时，可重试或换 raw.githubusercontent.com；知乎 403、jdon 有验证码。
- 用户偏好：可逆操作（改名保留而非删除）；未经要求不 commit；敏感信息（WiFi 密码等）不写入文档。

## Suggested skills

- `research`：继续核实 TClaw 微信通道性质、xiaoclaw 底座版本等文档事实（高可信来源+落盘结论）。
- `gitnexus-exploring` 或 Explore agent：clone 后分析 xiaoclaw 的 bridge 路由与 xiaozhi 侵入点（跨文件执行流追踪）。
- `diagnosing-bugs`：后续移植编译/启动失败时系统化定位。
- `code-review`：移植改动完成后审查相对用户 fork 的 diff。
- 一般编译烧录按 `HANDOFF-firmware-2026-11-19.md` 的环境命令执行，无需 skill。
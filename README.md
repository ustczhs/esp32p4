# ESP32-P4 语音助手工作区

基于 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的 ESP32-P4 语音助手固件定制，以及配套的主机侧工具与调研文档。

- 板卡：Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3（32MB Flash + 32MB PSRAM，WiFi 经 ESP32-C6 协处理器 / esp_hosted，仅 2.4GHz）
- 固件定制：微信 IM 桥接（esp-claw）、摄像头采集与视频流、双眼 GIF 表情动画、本地人脸追踪、字幕显示等
- 语音后端：自建服务（xiaozhi 协议 WebSocket/OTA），地址通过 `idf.py menuconfig` 配置

## 目录结构

```
xiaozhi-esp32/   固件（以 git subtree 并入，保留上游及本地定制的全部提交历史）
tools/           主机侧脚本：组件预取、串口抓包、微信扫码登录调试
HANDOFF*.md      各阶段交接记录（环境、编译烧录命令、待办）
PLAN-*.md        esp-claw × 微信融合方案设计与进度
```

`esp-claw-ref/`、`xiaoclaw-ref/` 为本地参考项目，`build/`、`managed_components/` 等构建产物及含会话凭据的调试日志均被 `.gitignore` 排除，不入库。

## 编译与烧录

需要 ESP-IDF v6.1：

```bash
source ~/esp/esp-idf-v6.1/export.sh
cd xiaozhi-esp32
idf.py set-target esp32p4
# 板型：menuconfig → Xiaozhi Assistant → Board Type
#   → Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3
# 启用 esp-claw 微信子系统时追加 claw.defaults 片段：
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32p4;claw.defaults" build
idf.py flash monitor
```

国内网络拉取 ESP 组件注册表超时的话，先运行 `tools/prefetch_components.py` 预取依赖（缓存于 `tools/claw_storage/`）。

更多细节见 [HANDOFF.md](HANDOFF.md) 与 [HANDOFF-firmware-2026-11-19.md](HANDOFF-firmware-2026-11-19.md)。

## 固件历史

`xiaozhi-esp32/` 通过 `git subtree` 并入本仓库，上游提交与本地定制提交均可用 `git log -- xiaozhi-esp32` 追溯。同步上游更新可在固件源仓库中 fetch 后再次 subtree pull。
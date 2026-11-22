#pragma once

// Returns true when the line is a claw CLI command (wechat/llm) and was handled.
bool WechatCliHandleLine(const char* line);

// Starts a standalone stdin reader only when the eye_cli task is not built in.
void WechatCliStart();
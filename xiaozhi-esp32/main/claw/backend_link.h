#pragma once

#include <esp_err.h>

// Independent WeChat control link to the backend (/wechat/v1/).
// Inbound messages are forwarded by the event router via the backend_forward
// cap; replies come back over the WS and are delivered to WeChat + announced.

void BackendLinkStart();
esp_err_t BackendLinkRegisterCap();
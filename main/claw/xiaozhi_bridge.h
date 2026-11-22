#pragma once

#include "claw_event_router.h"

// Outbound hook installed into claw_event_router_config_t. Observes final
// agent replies headed to an IM channel and feeds the xiaozhi announce path.
// Returning ESP_ERR_NOT_FOUND keeps the default channel binding (the reply
// is still sent to the originating chat).
esp_err_t XiaozhiBridgeOutboundResolver(const claw_event_t* event,
                                        const char* target_channel,
                                        const char* target_endpoint,
                                        char* cap_name,
                                        size_t cap_name_size,
                                        void* user_ctx);
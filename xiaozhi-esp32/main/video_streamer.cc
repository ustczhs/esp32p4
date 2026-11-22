#include "sdkconfig.h"

#if CONFIG_XIAOZHI_ENABLE_VIDEO_STREAM

#include "video_streamer.h"

#include "board.h"
#include "camera_hub.h"
#include "display.h"
#include "mcp_server.h"
#include "settings.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <wifi_manager.h>

#include <cstdio>
#include <cstring>

#include "esp_h264_enc_param.h"
#include "esp_h264_enc_single_hw.h"

#define TAG "VideoStreamer"

namespace {

constexpr size_t kMaxPublish = 256 * 1024;

// Prefer full sensor size; fall back when INTERNAL ref buffer cannot fit.
// ref@800?57KB, @640?46KB, @480?35KB (MALLOC_CAP_INTERNAL only).
struct EncPreset {
    uint16_t w;
    uint16_t h;
    int bitrate;
};

// Bitrate kept modest: 2.4GHz ESP-Hosted Wi-Fi stalls if TX queue fills (~2.5Mbps@800).
constexpr EncPreset kPresets[] = {
    {800, 800, 1200000},
    {640, 640, 1000000},
    {480, 480, 800000},
};

}  // namespace

void VideoStreamer::YuyvToUyvyInPlace(uint8_t* data, size_t len) {
    auto* p = reinterpret_cast<uint16_t*>(data);
    size_t count = len / 2;
    for (size_t i = 0; i < count; ++i) {
        p[i] = __builtin_bswap16(p[i]);
    }
}

void VideoStreamer::YuyvToOuYyEvYy(const uint8_t* src, int w, int h, uint8_t* dst) {
    const size_t stride = (size_t)w * 3 / 2;
    for (int y = 0; y < h; y += 2) {
        const uint8_t* row0 = src + (size_t)y * (size_t)w * 2;
        const uint8_t* row1 = src + (size_t)(y + 1) * (size_t)w * 2;
        uint8_t* out0 = dst + (size_t)y * stride;
        uint8_t* out1 = dst + (size_t)(y + 1) * stride;
        for (int x = 0; x < w; x += 2) {
            uint8_t y00 = row0[x * 2];
            uint8_t u0 = row0[x * 2 + 1];
            uint8_t y01 = row0[x * 2 + 2];
            uint8_t v0 = row0[x * 2 + 3];
            uint8_t y10 = row1[x * 2];
            uint8_t u1 = row1[x * 2 + 1];
            uint8_t y11 = row1[x * 2 + 2];
            uint8_t v1 = row1[x * 2 + 3];
            uint8_t u = (uint8_t)(((int)u0 + (int)u1) / 2);
            uint8_t v = (uint8_t)(((int)v0 + (int)v1) / 2);
            *out0++ = u;
            *out0++ = y00;
            *out0++ = y01;
            *out1++ = v;
            *out1++ = y10;
            *out1++ = y11;
        }
    }
}

bool VideoStreamer::ScaleYuyvNearest(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw,
                                     int dh) {
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || (dw & 1) || (sw & 1)) {
        return false;
    }
    if (sw < dw || sh < dh) {
        return false;
    }
    if (sw == dw && sh == dh) {
        memcpy(dst, src, (size_t)dw * (size_t)dh * 2);
        return true;
    }
    for (int y = 0; y < dh; ++y) {
        int sy = y * sh / dh;
        const uint8_t* srow = src + (size_t)sy * (size_t)sw * 2;
        uint8_t* drow = dst + (size_t)y * (size_t)dw * 2;
        for (int x = 0; x < dw; x += 2) {
            int sx = (x * sw / dw) & ~1;
            drow[x * 2 + 0] = srow[sx * 2 + 0];
            drow[x * 2 + 1] = srow[sx * 2 + 1];
            drow[x * 2 + 2] = srow[sx * 2 + 2];
            drow[x * 2 + 3] = srow[sx * 2 + 3];
        }
    }
    return true;
}

int VideoStreamer::BitrateFor(uint16_t width, uint16_t height) {
    for (const auto& p : kPresets) {
        if (p.w == width && p.h == height) {
            return p.bitrate;
        }
    }
    // ~0.4 bit/pixel/frame @12fps rough default
    return (int)((int64_t)width * height * 12 / 3);
}

void VideoStreamer::ReserveEncoderEarly() {
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "ReserveEncoderEarly: free_int=%u largest=%u", (unsigned)free_int,
             (unsigned)largest);
    if (!OpenBestEncoder()) {
        ESP_LOGW(TAG, "Early H.264 reserve failed; will retry when stream enables");
    }
}

void VideoStreamer::Start() {
    if (started_.exchange(true)) {
        return;
    }

    out_mutex_ = xSemaphoreCreateMutex();
    out_ready_ = xSemaphoreCreateBinary();
    if (!out_mutex_ || !out_ready_) {
        ESP_LOGE(TAG, "Failed to create sync primitives");
        started_.store(false);
        return;
    }

    CameraHub::GetInstance().Start();

    auto& mcp = McpServer::GetInstance();
    mcp.AddUserOnlyTool(
        "self.video.set_enabled",
        "Enable or disable local H.264 LAN preview stream "
        "(http://<device-ip>:8080/stream.h264). Default off. "
        "While streaming, self.camera.take_photo is disabled.",
        PropertyList({Property("enabled", kPropertyTypeBoolean)}),
        [this](const PropertyList& properties) -> ReturnValue {
            SetEnabled(properties["enabled"].value<bool>());
            return true;
        });
    mcp.AddUserOnlyTool("self.video.get_status",
                        "Get H.264 LAN stream status and URL hint.", PropertyList(),
                        [this](const PropertyList&) -> ReturnValue { return StatusJson(); });

    ESP_LOGI(TAG, "VideoStreamer ready (enc %ux%u). Will auto-enable after Wi-Fi.", enc_w_,
             enc_h_);

    xTaskCreate(
        [](void* arg) {
            auto* self = static_cast<VideoStreamer*>(arg);
            for (int i = 0; i < 60; ++i) {
                if (WifiManager::GetInstance().IsConnected()) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            vTaskDelay(pdMS_TO_TICKS(1500));
            if (WifiManager::GetInstance().IsConnected()) {
                ESP_LOGI(TAG, "Wi-Fi up, enabling H.264 LAN stream");
                self->SetEnabled(true);
            } else {
                ESP_LOGW(TAG, "Wi-Fi not connected; stream stays OFF");
            }
            vTaskDelete(nullptr);
        },
        "video_auto", 4096, this, 3, nullptr);
}

std::string VideoStreamer::StatusJson() const {
    std::string ip = "0.0.0.0";
    if (WifiManager::GetInstance().IsConnected()) {
        ip = WifiManager::GetInstance().GetIpAddress();
    }
    char buf[384];
    snprintf(buf, sizeof(buf),
             "{\"enabled\":%s,\"clients\":%d,\"frames\":%u,\"bytes\":%u,"
             "\"width\":%u,\"height\":%u,\"bitrate\":%d,"
             "\"url\":\"http://%s:%d/stream.h264\","
             "\"ffplay\":\"ffplay -fflags nobuffer -flags low_delay -f h264 -i "
             "http://%s:%d/stream.h264\"}",
             enabled_.load() ? "true" : "false", clients_.load(),
             (unsigned)frames_encoded_.load(), (unsigned)bytes_sent_.load(), (unsigned)enc_w_,
             (unsigned)enc_h_, bitrate_, ip.c_str(), kPort, ip.c_str(), kPort);
    return std::string(buf);
}

void VideoStreamer::SetEnabled(bool enabled) {
    if (enabled_.load() == enabled) {
        return;
    }

    auto display = Board::GetInstance().GetDisplay();
    if (!enabled) {
        enabled_.store(false);
        encode_running_.store(false);
        CameraHub::GetInstance().SetStreamConsumer(false);

        xSemaphoreGive(out_ready_);

        if (encode_task_) {
            for (int i = 0; i < 50 && encode_task_ != nullptr; ++i) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        StopHttpServer();
        // Keep encoder reserved so re-enable does not lose the high-res INTERNAL slot.

        Settings settings("video", true);
        settings.SetBool("enabled", false);
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        if (display) {
            display->ShowNotification("video: OFF", 2000);
        }
        ESP_LOGI(TAG, "Video stream disabled (encoder kept %ux%u)", enc_w_, enc_h_);
        return;
    }

    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (enc_ == nullptr) {
        if (!OpenBestEncoder()) {
            if (display) {
                display->ShowNotification("video: enc OOM", 3000);
            }
            return;
        }
    }

    if (!StartHttpServer()) {
        if (display) {
            display->ShowNotification("video: HTTP fail", 3000);
        }
        return;
    }

    enabled_.store(true);
    encode_running_.store(true);
    CameraHub::GetInstance().SetStreamConsumer(true);
    frames_encoded_.store(0);
    bytes_sent_.store(0);

    BaseType_t ok =
        xTaskCreatePinnedToCore(EncodeTaskEntry, "h264_enc", 8192, this, 5, &encode_task_, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create encode task");
        encode_task_ = nullptr;
        enabled_.store(false);
        encode_running_.store(false);
        CameraHub::GetInstance().SetStreamConsumer(false);
        StopHttpServer();
        return;
    }

    Settings settings("video", true);
    settings.SetBool("enabled", true);

    std::string ip = WifiManager::GetInstance().IsConnected()
                         ? WifiManager::GetInstance().GetIpAddress()
                         : std::string("?");
    char tip[96];
    snprintf(tip, sizeof(tip), "video: %ux%u", enc_w_, enc_h_);
    if (display) {
        display->ShowNotification(tip, 4000);
    }
    ESP_LOGI(TAG, "Video stream ON -> http://%s:%d/stream.h264 (%ux%u @%dbps)", ip.c_str(), kPort,
             enc_w_, enc_h_, bitrate_);
}

bool VideoStreamer::OpenBestEncoder() {
    for (const auto& p : kPresets) {
        if (OpenEncoder(p.w, p.h, p.bitrate)) {
            return true;
        }
        ESP_LOGW(TAG, "Encoder %ux%u failed, trying next preset", p.w, p.h);
    }
    return false;
}

bool VideoStreamer::OpenEncoder(uint16_t width, uint16_t height, int bitrate) {
    CloseEncoder();

    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "OpenEncoder %ux%u: free_int=%u largest=%u bitrate=%d", width, height,
             (unsigned)free_int, (unsigned)largest, bitrate);

    esp_h264_enc_cfg_hw_t cfg = {};
#if CONFIG_ESP_REV_MIN_FULL >= 300
    cfg.pic_type = ESP_H264_RAW_FMT_UYVY;
#else
    cfg.pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY;
#endif
    cfg.gop = kGop;
    cfg.fps = kFps;
    cfg.res = {.width = width, .height = height};
    // Lower qp_min a bit for sharper stills at the given bitrate.
    cfg.rc = {.bitrate = (uint32_t)bitrate, .qp_min = 24, .qp_max = 36};

    esp_h264_enc_handle_t enc = nullptr;
    if (esp_h264_enc_hw_new(&cfg, &enc) != ESP_H264_ERR_OK || enc == nullptr) {
        ESP_LOGE(TAG, "esp_h264_enc_hw_new failed (free_int=%u largest=%u)", (unsigned)free_int,
                 (unsigned)largest);
        return false;
    }
    if (esp_h264_enc_open(enc) != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "esp_h264_enc_open failed");
        esp_h264_enc_del(enc);
        return false;
    }
    enc_ = enc;

    esp_h264_enc_param_hw_handle_t hw_param = nullptr;
    if (esp_h264_enc_hw_get_param_hd(enc, &hw_param) == ESP_H264_ERR_OK) {
        enc_param_ = hw_param;
    }

    float bpp = ESP_H264_GET_BPP_BY_PIC_TYPE(cfg.pic_type);
    uint32_t need = (uint32_t)((float)width * (float)height * bpp);
    in_buf_size_ = need;
    out_buf_size_ = need;
    in_buf_ = (uint8_t*)heap_caps_aligned_calloc(64, 1, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    out_buf_ = (uint8_t*)heap_caps_aligned_calloc(64, 1, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!in_buf_ || !out_buf_) {
        ESP_LOGE(TAG, "encoder buffer alloc failed");
        CloseEncoder();
        return false;
    }

    enc_w_ = width;
    enc_h_ = height;
    bitrate_ = bitrate;
    ESP_LOGI(TAG, "H264 encoder %ux%u @%dfps %dbps pic=0x%08lx", width, height, kFps, bitrate,
             (unsigned long)cfg.pic_type);
    return true;
}

void VideoStreamer::CloseEncoder() {
    if (enc_) {
        auto* enc = static_cast<esp_h264_enc_handle_t>(enc_);
        esp_h264_enc_close(enc);
        esp_h264_enc_del(enc);
        enc_ = nullptr;
    }
    enc_param_ = nullptr;
    if (in_buf_) {
        heap_caps_free(in_buf_);
        in_buf_ = nullptr;
        in_buf_size_ = 0;
    }
    if (out_buf_) {
        heap_caps_free(out_buf_);
        out_buf_ = nullptr;
        out_buf_size_ = 0;
    }
    enc_w_ = 0;
    enc_h_ = 0;
}

bool VideoStreamer::StartHttpServer() {
    if (httpd_) {
        return true;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kPort;
    config.ctrl_port = 32770;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 2;
    // Keep short: a stalled client must not wedge the Wi-Fi TX path for long.
    config.send_wait_timeout = 1;

    if (httpd_start(&httpd_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed on port %d", kPort);
        httpd_ = nullptr;
        return false;
    }

    httpd_uri_t uri = {
        .uri = "/stream.h264",
        .method = HTTP_GET,
        .handler = StreamGetHandler,
        .user_ctx = this,
    };
    httpd_register_uri_handler(httpd_, &uri);
    ESP_LOGI(TAG, "HTTP server on :%d /stream.h264", kPort);
    return true;
}

void VideoStreamer::StopHttpServer() {
    if (httpd_) {
        httpd_stop(httpd_);
        httpd_ = nullptr;
    }
    clients_.store(0);
    send_busy_.store(false);
    if (send_scratch_) {
        heap_caps_free(send_scratch_);
        send_scratch_ = nullptr;
        send_scratch_cap_ = 0;
    }
}

void VideoStreamer::PublishEncoded(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0 || clients_.load() <= 0) {
        return;
    }
    xSemaphoreTake(out_mutex_, portMAX_DELAY);
    if (publish_cap_ < len) {
        if (publish_buf_) {
            heap_caps_free(publish_buf_);
            publish_buf_ = nullptr;
            publish_cap_ = 0;
        }
        size_t cap = len < kMaxPublish ? len : kMaxPublish;
        if (len > kMaxPublish) {
            len = kMaxPublish;
        }
        publish_buf_ = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        publish_cap_ = publish_buf_ ? cap : 0;
    }
    if (publish_buf_ && len <= publish_cap_) {
        memcpy(publish_buf_, data, len);
        publish_len_ = len;
        publish_seq_++;
        bytes_sent_.fetch_add((uint32_t)len);
    }
    xSemaphoreGive(out_mutex_);
    // Always wake sender; if it is mid-send it will pick up the latest seq next.
    xSemaphoreGive(out_ready_);
}

esp_err_t VideoStreamer::StreamGetHandler(httpd_req_t* req) {
    auto* self = static_cast<VideoStreamer*>(req->user_ctx);
    if (self == nullptr || !self->enabled_.load()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "stream disabled");
        return ESP_FAIL;
    }

    if (self->clients_.load() > 0) {
        self->kick_client_.store(true);
        xSemaphoreGive(self->out_ready_);
        for (int i = 0; i < 60 && self->clients_.load() > 0; ++i) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        if (self->clients_.load() > 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy: one client only");
            return ESP_FAIL;
        }
    }
    self->kick_client_.store(false);
    self->clients_.store(1);
    self->force_idr_.store(true);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    httpd_resp_set_hdr(req, "Connection", "close");

    ESP_LOGI(TAG, "Client connected to /stream.h264");
    uint32_t last_seq = 0;
    while (self->enabled_.load()) {
        if (self->kick_client_.load()) {
            ESP_LOGI(TAG, "Client kicked by new connection");
            break;
        }
        if (xSemaphoreTake(self->out_ready_, pdMS_TO_TICKS(300)) != pdTRUE) {
            continue;
        }
        if (!self->enabled_.load() || self->kick_client_.load()) {
            break;
        }

        // Copy under lock, then send WITHOUT holding out_mutex_ so encode can keep
        // dropping-to-latest instead of blocking the whole pipeline.
        size_t len = 0;
        uint32_t seq = 0;
        xSemaphoreTake(self->out_mutex_, portMAX_DELAY);
        seq = self->publish_seq_;
        if (seq != last_seq && self->publish_buf_ && self->publish_len_ > 0) {
            len = self->publish_len_;
            if (self->send_scratch_cap_ < len) {
                if (self->send_scratch_) {
                    heap_caps_free(self->send_scratch_);
                    self->send_scratch_ = nullptr;
                    self->send_scratch_cap_ = 0;
                }
                size_t cap = len < kMaxPublish ? len : kMaxPublish;
                self->send_scratch_ =
                    (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                self->send_scratch_cap_ = self->send_scratch_ ? cap : 0;
            }
            if (self->send_scratch_ && len <= self->send_scratch_cap_) {
                memcpy(self->send_scratch_, self->publish_buf_, len);
                last_seq = seq;
            } else {
                len = 0;
            }
        }
        xSemaphoreGive(self->out_mutex_);

        if (len == 0 || self->send_scratch_ == nullptr) {
            continue;
        }

        self->send_busy_.store(true);
        esp_err_t err =
            httpd_resp_send_chunk(req, (const char*)self->send_scratch_, len);
        self->send_busy_.store(false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Client send failed: %s", esp_err_to_name(err));
            break;
        }
    }

    httpd_resp_send_chunk(req, nullptr, 0);
    self->send_busy_.store(false);
    self->kick_client_.store(false);
    self->clients_.store(0);
    ESP_LOGI(TAG, "Client disconnected");
    return ESP_OK;
}

void VideoStreamer::EncodeTaskEntry(void* arg) {
    static_cast<VideoStreamer*>(arg)->EncodeLoop();
    static_cast<VideoStreamer*>(arg)->encode_task_ = nullptr;
    vTaskDelete(nullptr);
}

void VideoStreamer::EncodeLoop() {
    auto& hub = CameraHub::GetInstance();
    uint32_t last_seq = 0;
    int idle_log = 0;
    int open_fail = 0;

    while (encode_running_.load()) {
        CameraFrame frame;
        uint32_t seq = 0;
        if (!hub.AcquireLatest(&frame, &seq, last_seq, pdMS_TO_TICKS(500))) {
            continue;
        }
        last_seq = seq;

        if (enc_w_ == 0 || enc_h_ == 0 || frame.width < enc_w_ || frame.height < enc_h_ ||
            frame.data == nullptr) {
            hub.ReleaseFrame(seq);
            continue;
        }

        if (enc_ == nullptr) {
            if (!OpenBestEncoder()) {
                hub.ReleaseFrame(seq);
                open_fail++;
                vTaskDelay(pdMS_TO_TICKS(open_fail < 5 ? 500 : 3000));
                continue;
            }
            open_fail = 0;
        }

        size_t packed = (size_t)enc_w_ * (size_t)enc_h_ * 2;
#if CONFIG_ESP_REV_MIN_FULL < 300
        size_t need = (size_t)enc_w_ * (size_t)enc_h_ * 3 / 2;
#else
        size_t need = packed;
#endif
        if (need > in_buf_size_) {
            ESP_LOGE(TAG, "frame too large %u > %u", (unsigned)need, (unsigned)in_buf_size_);
            hub.ReleaseFrame(seq);
            continue;
        }

#if CONFIG_ESP_REV_MIN_FULL >= 300
        if (!ScaleYuyvNearest(frame.data, frame.width, frame.height, in_buf_, enc_w_, enc_h_)) {
            hub.ReleaseFrame(seq);
            continue;
        }
        hub.ReleaseFrame(seq);
#if CONFIG_XIAOZHI_ENABLE_CAMERA_ENDIANNESS_SWAP
        YuyvToUyvyInPlace(in_buf_, packed);
#endif
#else
        if (!out_buf_ || out_buf_size_ < packed) {
            hub.ReleaseFrame(seq);
            continue;
        }
        if (!ScaleYuyvNearest(frame.data, frame.width, frame.height, out_buf_, enc_w_, enc_h_)) {
            hub.ReleaseFrame(seq);
            continue;
        }
        hub.ReleaseFrame(seq);
        YuyvToOuYyEvYy(out_buf_, enc_w_, enc_h_, in_buf_);
#endif

        if (force_idr_.exchange(false) && enc_param_) {
            esp_h264_enc_force_idr(static_cast<esp_h264_enc_param_handle_t>(enc_param_));
        }

        esp_h264_enc_in_frame_t in_frame = {};
        in_frame.raw_data.buffer = in_buf_;
        in_frame.raw_data.len = in_buf_size_;
        in_frame.pts = (uint32_t)(esp_timer_get_time() / 1000);

        esp_h264_enc_out_frame_t out_frame = {};
        out_frame.raw_data.buffer = out_buf_;
        out_frame.raw_data.len = out_buf_size_;

        auto* enc = static_cast<esp_h264_enc_handle_t>(enc_);
        esp_h264_err_t ret = esp_h264_enc_process(enc, &in_frame, &out_frame);
        if (ret != ESP_H264_ERR_OK) {
            if (++idle_log % 30 == 1) {
                ESP_LOGW(TAG, "encode failed: %d", (int)ret);
            }
            continue;
        }

        frames_encoded_.fetch_add(1);
        if (clients_.load() > 0 && out_frame.length > 0) {
            PublishEncoded(out_frame.raw_data.buffer, out_frame.length);
        }

        if (frames_encoded_.load() % 36 == 1) {
            ESP_LOGI(TAG, "encoded=%u last=%uB clients=%d %ux%u", (unsigned)frames_encoded_.load(),
                     (unsigned)out_frame.length, clients_.load(), enc_w_, enc_h_);
        }
    }

    // Encoder stays open (reserved) across stop/start.
    ESP_LOGI(TAG, "Encode loop exit");
}

#endif  // CONFIG_XIAOZHI_ENABLE_VIDEO_STREAM

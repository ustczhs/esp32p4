#include "sdkconfig.h"

#if CONFIG_XIAOZHI_ENABLE_FACE_TRACK

#include "face_tracker.h"

#include "application.h"
#include "board.h"
#include "camera.h"
#include "device_state.h"
#include "display.h"
#include "mcp_server.h"
#include "settings.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <linux/videodev2.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "dl_image_define.hpp"
#include "human_face_detect.hpp"

#define TAG "FaceTracker"

namespace {

constexpr int kDetectWidth = 240;
constexpr int kDetectHeight = 240;
constexpr int kLoopDelayMs = 250;
constexpr int kLostFramesToClear = 2;
constexpr float kSmoothAlpha = 0.35f;

static inline uint16_t YuvToRgb565(int y, int u, int v) {
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void SampleYuyvToRgb565(const uint8_t* src, int src_w, int src_h, uint16_t* dst, int dst_w,
                               int dst_h) {
    for (int y = 0; y < dst_h; ++y) {
        int sy = y * src_h / dst_h;
        const uint8_t* row = src + (size_t)sy * (size_t)src_w * 2;
        for (int x = 0; x < dst_w; ++x) {
            int sx = x * src_w / dst_w;
            int pair = (sx & ~1) * 2;
            uint8_t y0 = row[pair];
            uint8_t u = row[pair + 1];
            uint8_t y1 = row[pair + 2];
            uint8_t v = row[pair + 3];
            int yy = (sx & 1) ? y1 : y0;
            dst[y * dst_w + x] = YuvToRgb565(yy, u, v);
        }
    }
}

static void SampleUyvyToRgb565(const uint8_t* src, int src_w, int src_h, uint16_t* dst, int dst_w,
                               int dst_h) {
    for (int y = 0; y < dst_h; ++y) {
        int sy = y * src_h / dst_h;
        const uint8_t* row = src + (size_t)sy * (size_t)src_w * 2;
        for (int x = 0; x < dst_w; ++x) {
            int sx = x * src_w / dst_w;
            int pair = (sx & ~1) * 2;
            uint8_t u = row[pair];
            uint8_t y0 = row[pair + 1];
            uint8_t v = row[pair + 2];
            uint8_t y1 = row[pair + 3];
            int yy = (sx & 1) ? y1 : y0;
            dst[y * dst_w + x] = YuvToRgb565(yy, u, v);
        }
    }
}

static void SampleRgb565(const uint16_t* src, int src_w, int src_h, uint16_t* dst, int dst_w,
                         int dst_h) {
    for (int y = 0; y < dst_h; ++y) {
        int sy = y * src_h / dst_h;
        for (int x = 0; x < dst_w; ++x) {
            int sx = x * src_w / dst_w;
            dst[y * dst_w + x] = src[sy * src_w + sx];
        }
    }
}


static void DrawHLine(uint16_t* img, int w, int h, int y, int x0, int x1, uint16_t color) {
    if (y < 0 || y >= h) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x0 < 0) x0 = 0;
    if (x1 >= w) x1 = w - 1;
    for (int x = x0; x <= x1; ++x) img[y * w + x] = color;
}

static void DrawVLine(uint16_t* img, int w, int h, int x, int y0, int y1, uint16_t color) {
    if (x < 0 || x >= w) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y0 < 0) y0 = 0;
    if (y1 >= h) y1 = h - 1;
    for (int y = y0; y <= y1; ++y) img[y * w + x] = color;
}

static void DrawBox(uint16_t* img, int w, int h, int x0, int y0, int x1, int y1, uint16_t color) {
    DrawHLine(img, w, h, y0, x0, x1, color);
    DrawHLine(img, w, h, y1, x0, x1, color);
    DrawVLine(img, w, h, x0, y0, y1, color);
    DrawVLine(img, w, h, x1, y0, y1, color);
}

}  // namespace

void FaceTracker::Start() {
    if (started_.exchange(true)) {
        return;
    }

    Settings settings("face", false);
    enabled_.store(settings.GetBool("enabled", true));
    preview_enabled_.store(settings.GetBool("preview", true));

    auto& mcp = McpServer::GetInstance();
    mcp.AddUserOnlyTool(
        "self.face_track.set_enabled",
        "Enable or disable local face tracking. When enabled, the device detects faces on-device "
        "and shows center coordinates.",
        PropertyList({Property("enabled", kPropertyTypeBoolean)}),
        [this](const PropertyList& properties) -> ReturnValue {
            SetEnabled(properties["enabled"].value<bool>());
            return true;
        });
    mcp.AddUserOnlyTool(
        "self.face_track.set_preview",
        "Enable or disable the low-resolution live debug preview overlay on screen.",
        PropertyList({Property("enabled", kPropertyTypeBoolean)}),
        [this](const PropertyList& properties) -> ReturnValue {
            SetPreviewEnabled(properties["enabled"].value<bool>());
            return true;
        });
    mcp.AddUserOnlyTool("self.face_track.get_status",
                        "Get face tracking status and latest face center coordinates.",
                        PropertyList(), [this](const PropertyList&) -> ReturnValue {
                            char buf[256];
                            if (HasFace()) {
                                snprintf(buf, sizeof(buf),
                                         "{\"enabled\":%s,\"preview\":%s,\"has_face\":true,"
                                         "\"cx\":%d,\"cy\":%d,\"frame_w\":%d,\"frame_h\":%d}",
                                         IsEnabled() ? "true" : "false",
                                         IsPreviewEnabled() ? "true" : "false", CenterX(),
                                         CenterY(), FrameWidth(), FrameHeight());
                            } else {
                                snprintf(buf, sizeof(buf),
                                         "{\"enabled\":%s,\"preview\":%s,\"has_face\":false,"
                                         "\"cx\":null,\"cy\":null,\"frame_w\":%d,\"frame_h\":%d}",
                                         IsEnabled() ? "true" : "false",
                                         IsPreviewEnabled() ? "true" : "false", FrameWidth(),
                                         FrameHeight());
                            }
                            return std::string(buf);
                        });
    mcp.AddUserOnlyTool(
        "self.audio.set_enabled",
        "Enable or disable voice pipeline (wake word + listening). Video/face tracking is "
        "independent.",
        PropertyList({Property("enabled", kPropertyTypeBoolean)}),
        [](const PropertyList& properties) -> ReturnValue {
            bool enable = properties["enabled"].value<bool>();
            auto& app = Application::GetInstance();
            auto& audio = app.GetAudioService();
            if (enable) {
                audio.EnableWakeWordDetection(true);
            } else {
                audio.EnableVoiceProcessing(false);
                audio.EnableWakeWordDetection(false);
                if (app.GetDeviceState() == kDeviceStateListening ||
                    app.GetDeviceState() == kDeviceStateSpeaking) {
                    app.SetDeviceState(kDeviceStateIdle);
                }
            }
            Settings settings("audio", true);
            settings.SetBool("enabled", enable);
            return true;
        });

    BaseType_t ok =
        xTaskCreatePinnedToCore(TaskEntry, "face_track", 8192, this, 3, nullptr, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create face_track task");
        started_.store(false);
    }
}

void FaceTracker::SetEnabled(bool enabled) {
    enabled_.store(enabled);
    Settings settings("face", true);
    settings.SetBool("enabled", enabled);
    auto display = Board::GetInstance().GetDisplay();
    if (!enabled) {
        has_face_.store(false);
        if (display) {
            display->ClearDebugPreview();
            display->ShowNotification("face: OFF", 2000);
        }
    } else if (display) {
        display->ShowNotification("face: ON", 2000);
    }
}

void FaceTracker::SetPreviewEnabled(bool enabled) {
    preview_enabled_.store(enabled);
    Settings settings("face", true);
    settings.SetBool("preview", enabled);
    auto display = Board::GetInstance().GetDisplay();
    if (!enabled && display) {
        display->ClearDebugPreview();
        display->ShowNotification("preview: OFF", 1500);
    } else if (display) {
        display->ShowNotification("preview: ON", 1500);
    }
}

void FaceTracker::TaskEntry(void* arg) {
    static_cast<FaceTracker*>(arg)->RunLoop();
    vTaskDelete(nullptr);
}

void FaceTracker::RunLoop() {
    // Wait until camera/board settle.
    {
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            display->ShowNotification("face: starting...", 3000);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

    std::unique_ptr<HumanFaceDetect> detector =
        std::make_unique<HumanFaceDetect>(HumanFaceDetect::MSRMNP_S8_V1, false);
    if (detector == nullptr) {
        ESP_LOGE(TAG, "HumanFaceDetect init failed");
        started_.store(false);
        return;
    }

    uint16_t* scaled = (uint16_t*)heap_caps_malloc(kDetectWidth * kDetectHeight * sizeof(uint16_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scaled) {
        ESP_LOGE(TAG, "Failed to allocate scaled buffer");
        started_.store(false);
        return;
    }

    ESP_LOGI(TAG, "Face tracker started (MSR+MNP @ %dx%d, %dms)", kDetectWidth, kDetectHeight,
             kLoopDelayMs);

    int miss = 0;
    float smooth_x = 0;
    float smooth_y = 0;
    bool smoothed = false;
    int idle_log = 0;

    while (true) {
        if (!enabled_.load()) {
            auto display = Board::GetInstance().GetDisplay();
            if (display) {
                display->ClearDebugPreview();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        auto camera = Board::GetInstance().GetCamera();
        if (camera == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        CameraFrame frame;
        int64_t t0 = esp_timer_get_time();
        if (!camera->GrabFrame(&frame, false) || frame.data == nullptr || frame.width == 0 ||
            frame.height == 0) {
            if (++idle_log % 20 == 1) {
                ESP_LOGW(TAG, "GrabFrame failed");
            }
            vTaskDelay(pdMS_TO_TICKS(kLoopDelayMs));
            continue;
        }

        frame_w_.store(frame.width);
        frame_h_.store(frame.height);

        bool ok_scale = false;
#if CONFIG_XIAOZHI_ENABLE_CAMERA_ENDIANNESS_SWAP
        // GrabFrame applies bswap16 on packed pixels: UYVY bytes become YUYV layout.
        // Sample the post-swap layout. (An older fallback tried the other layout for
        // detection and left it on screen ? green/magenta cast.)
        const uint32_t sample_fmt =
            (frame.format == V4L2_PIX_FMT_UYVY)   ? V4L2_PIX_FMT_YUYV
            : (frame.format == V4L2_PIX_FMT_YUYV) ? V4L2_PIX_FMT_UYVY
                                                 : frame.format;
#else
        const uint32_t sample_fmt = frame.format;
#endif
        auto scale_to = [&](uint32_t fmt) {
            if (fmt == V4L2_PIX_FMT_YUYV || fmt == V4L2_PIX_FMT_YUV422P) {
                SampleYuyvToRgb565(frame.data, frame.width, frame.height, scaled, kDetectWidth,
                                   kDetectHeight);
                return true;
            }
            if (fmt == V4L2_PIX_FMT_UYVY) {
                SampleUyvyToRgb565(frame.data, frame.width, frame.height, scaled, kDetectWidth,
                                   kDetectHeight);
                return true;
            }
            if (fmt == V4L2_PIX_FMT_RGB565) {
                SampleRgb565(reinterpret_cast<const uint16_t*>(frame.data), frame.width,
                             frame.height, scaled, kDetectWidth, kDetectHeight);
                return true;
            }
            return false;
        };

        if (!scale_to(sample_fmt)) {
            ESP_LOGW(TAG, "Unsupported format 0x%08lx for face track", (unsigned long)frame.format);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ok_scale = true;
        (void)ok_scale;

        dl::image::img_t img = {
            .data = scaled,
            .width = static_cast<uint16_t>(kDetectWidth),
            .height = static_cast<uint16_t>(kDetectHeight),
            .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
        };

        auto* results = &detector->run(img);
        int64_t cost_ms = (esp_timer_get_time() - t0) / 1000;

        char msg[48];
        auto display = Board::GetInstance().GetDisplay();
        constexpr uint16_t kGreen = 0x07E0;
        constexpr uint16_t kRed = 0xF800;

        if (!results->empty()) {
            // Pick largest face.
            const dl::detect::result_t* best = &results->front();
            int best_area = best->box_area();
            for (const auto& r : *results) {
                int area = r.box_area();
                if (area > best_area) {
                    best = &r;
                    best_area = area;
                }
            }

            int bx0 = (int)best->box[0];
            int by0 = (int)best->box[1];
            int bx1 = (int)best->box[2];
            int by1 = (int)best->box[3];
            DrawBox(scaled, kDetectWidth, kDetectHeight, bx0, by0, bx1, by1, kGreen);
            int cx_i = (bx0 + bx1) / 2;
            int cy_i = (by0 + by1) / 2;
            DrawHLine(scaled, kDetectWidth, kDetectHeight, cy_i, cx_i - 4, cx_i + 4, kGreen);
            DrawVLine(scaled, kDetectWidth, kDetectHeight, cx_i, cy_i - 4, cy_i + 4, kGreen);

            float cx_s = (best->box[0] + best->box[2]) * 0.5f;
            float cy_s = (best->box[1] + best->box[3]) * 0.5f;
            // Map detect coords back to original frame.
            float cx = cx_s * frame.width / kDetectWidth;
            float cy = cy_s * frame.height / kDetectHeight;

            if (!smoothed) {
                smooth_x = cx;
                smooth_y = cy;
                smoothed = true;
            } else {
                smooth_x = smooth_x * (1.0f - kSmoothAlpha) + cx * kSmoothAlpha;
                smooth_y = smooth_y * (1.0f - kSmoothAlpha) + cy * kSmoothAlpha;
            }

            int ix = (int)std::lround(smooth_x);
            int iy = (int)std::lround(smooth_y);
            center_x_.store(ix);
            center_y_.store(iy);
            has_face_.store(true);
            miss = 0;

            snprintf(msg, sizeof(msg), "face (%d,%d)", ix, iy);
            if (display) {
                display->ShowNotification(msg, 800);
            }
            ESP_LOGI(TAG, "face center=(%d,%d) score=%.2f cost=%lldms", ix, iy, best->score,
                     (long long)cost_ms);
        } else {
            miss++;
            if (miss >= kLostFramesToClear) {
                has_face_.store(false);
                smoothed = false;
            }
            // Center cross so user can tell preview is live.
            DrawHLine(scaled, kDetectWidth, kDetectHeight, kDetectHeight / 2, 0, kDetectWidth - 1,
                      kRed);
            DrawVLine(scaled, kDetectWidth, kDetectHeight, kDetectWidth / 2, 0, kDetectHeight - 1,
                      kRed);
            snprintf(msg, sizeof(msg), "face (nan,nan)");
            if (display) {
                display->ShowNotification(msg, 800);
            }
            if (++idle_log % 8 == 1) {
                // Rough brightness probe: helps diagnose black preview vs UI bug.
                uint32_t sum = 0;
                const int n = kDetectWidth * kDetectHeight;
                for (int i = 0; i < n; i += 16) {
                    uint16_t p = scaled[i];
                    int r = (p >> 11) & 0x1F;
                    int g = (p >> 5) & 0x3F;
                    int b = p & 0x1F;
                    sum += (r * 2 + g + b);
                }
                ESP_LOGI(TAG, "face (nan,nan) %dx%d cost=%lldms bright~%lu", frame.width,
                         frame.height, (long long)cost_ms, (unsigned long)(sum / (n / 16 + 1)));
            }
        }

        if (display && preview_enabled_.load()) {
            display->SetDebugPreviewRgb565(scaled, kDetectWidth, kDetectHeight, msg);
        }

        int remain = kLoopDelayMs - (int)cost_ms;
        if (remain < 20) {
            remain = 20;
        }
        vTaskDelay(pdMS_TO_TICKS(remain));
    }
}

#endif  // CONFIG_XIAOZHI_ENABLE_FACE_TRACK

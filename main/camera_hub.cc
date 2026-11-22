#include "sdkconfig.h"

#if CONFIG_XIAOZHI_ENABLE_FACE_TRACK || CONFIG_XIAOZHI_ENABLE_VIDEO_STREAM

#include "camera_hub.h"

#include "board.h"
#include "camera.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <cstring>

#define TAG "CameraHub"

void CameraHub::Start() {
    if (started_.exchange(true)) {
        return;
    }

    frame_sem_ = xSemaphoreCreateBinary();
    buffer_mutex_ = xSemaphoreCreateMutex();
    if (!frame_sem_ || !buffer_mutex_) {
        ESP_LOGE(TAG, "Failed to create sync primitives");
        started_.store(false);
        return;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(TaskEntry, "cam_hub", 6144, this, 4, nullptr, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create cam_hub task");
        started_.store(false);
        return;
    }
    ESP_LOGI(TAG, "CameraHub started");
}

void CameraHub::SetFaceConsumer(bool on) {
    face_wanted_.store(on);
}

void CameraHub::SetStreamConsumer(bool on) {
    stream_wanted_.store(on);
}

int CameraHub::PeriodMs() const {
    if (stream_wanted_.load()) {
        return 83;  // ~12 fps
    }
    return 250;  // face-only ~4 fps
}

void CameraHub::EnsureBuffer(size_t need) {
    if (capacity_ >= need && buffer_ != nullptr) {
        return;
    }
    if (buffer_) {
        heap_caps_free(buffer_);
        buffer_ = nullptr;
        capacity_ = 0;
    }
    buffer_ = (uint8_t*)heap_caps_aligned_alloc(64, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer_) {
        capacity_ = need;
    }
}

void CameraHub::TaskEntry(void* arg) {
    static_cast<CameraHub*>(arg)->CaptureLoop();
    vTaskDelete(nullptr);
}

void CameraHub::CaptureLoop() {
    // Let camera settle with the rest of the board.
    vTaskDelay(pdMS_TO_TICKS(1500));

    while (true) {
        if (!HasActiveConsumers()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        int64_t t0 = esp_timer_get_time();
        auto camera = Board::GetInstance().GetCamera();
        if (camera == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // Wait for consumers to release the previous frame (drop if stuck).
        for (int i = 0; i < 20 && refs_.load() > 0; ++i) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (refs_.load() > 0) {
            ESP_LOGW(TAG, "Dropping frame: consumers still hold refs=%d", refs_.load());
            // Force-clear so capture can continue; late ReleaseFrame is ignored by seq check.
            refs_.store(0);
        }

        CameraFrame frame;
        if (!camera->GrabFrame(&frame, false) || frame.data == nullptr || frame.width == 0 ||
            frame.height == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        xSemaphoreTake(buffer_mutex_, portMAX_DELAY);
        EnsureBuffer(frame.len);
        if (buffer_ == nullptr) {
            xSemaphoreGive(buffer_mutex_);
            ESP_LOGE(TAG, "Hub buffer alloc failed (%u bytes)", (unsigned)frame.len);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        memcpy(buffer_, frame.data, frame.len);
        len_ = frame.len;
        width_ = frame.width;
        height_ = frame.height;
        format_ = frame.format;
        uint32_t seq = latest_seq_.load() + 1;
        if (seq == 0) {
            seq = 1;
        }
        latest_seq_.store(seq);
        refs_.store(0);
        xSemaphoreGive(buffer_mutex_);

        // Wake waiters (binary sem: give once; AcquireLatest re-checks seq).
        xSemaphoreGive(frame_sem_);

        int remain = PeriodMs() - (int)((esp_timer_get_time() - t0) / 1000);
        if (remain < 5) {
            remain = 5;
        }
        vTaskDelay(pdMS_TO_TICKS(remain));
    }
}

bool CameraHub::AcquireLatest(CameraFrame* out, uint32_t* seq, uint32_t after_seq, TickType_t ticks) {
    if (out == nullptr || seq == nullptr || !started_.load()) {
        return false;
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t left = ticks;
    while (true) {
        uint32_t cur = latest_seq_.load();
        if (cur > after_seq) {
            xSemaphoreTake(buffer_mutex_, portMAX_DELAY);
            cur = latest_seq_.load();
            if (cur > after_seq && buffer_ != nullptr && len_ > 0) {
                out->data = buffer_;
                out->len = len_;
                out->width = width_;
                out->height = height_;
                out->format = format_;
                *seq = cur;
                refs_.fetch_add(1);
                xSemaphoreGive(buffer_mutex_);
                return true;
            }
            xSemaphoreGive(buffer_mutex_);
        }

        if (ticks != portMAX_DELAY) {
            TickType_t elapsed = xTaskGetTickCount() - start;
            if (elapsed >= ticks) {
                return false;
            }
            left = ticks - elapsed;
        }
        if (xSemaphoreTake(frame_sem_, left) != pdTRUE) {
            return false;
        }
    }
}

void CameraHub::ReleaseFrame(uint32_t seq) {
    if (seq == 0) {
        return;
    }
    if (seq == latest_seq_.load()) {
        int v = refs_.fetch_sub(1);
        if (v <= 0) {
            refs_.store(0);
        }
    }
}

#endif  // CONFIG_XIAOZHI_ENABLE_FACE_TRACK || CONFIG_XIAOZHI_ENABLE_VIDEO_STREAM

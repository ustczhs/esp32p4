#pragma once

#include "camera.h"

#include <atomic>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Single capture fan-out: one GrabFrame, multiple consumers (face / video stream).
class CameraHub {
public:
    static CameraHub& GetInstance() {
        static CameraHub instance;
        return instance;
    }

    void Start();

    void SetFaceConsumer(bool on);
    void SetStreamConsumer(bool on);

    // Wait until a frame with seq > after_seq is published.
    // On success, out->data points at the shared hub buffer; call ReleaseFrame(seq) promptly.
    bool AcquireLatest(CameraFrame* out, uint32_t* seq, uint32_t after_seq, TickType_t ticks);
    void ReleaseFrame(uint32_t seq);

    uint32_t CurrentSeq() const { return latest_seq_.load(); }
    bool HasActiveConsumers() const {
        return face_wanted_.load() || stream_wanted_.load();
    }

private:
    CameraHub() = default;
    static void TaskEntry(void* arg);
    void CaptureLoop();
    void EnsureBuffer(size_t need);
    int PeriodMs() const;

    std::atomic<bool> started_{false};
    std::atomic<bool> face_wanted_{false};
    std::atomic<bool> stream_wanted_{false};
    std::atomic<uint32_t> latest_seq_{0};
    std::atomic<int> refs_{0};

    uint8_t* buffer_ = nullptr;
    size_t capacity_ = 0;
    size_t len_ = 0;
    uint16_t width_ = 0;
    uint16_t height_ = 0;
    uint32_t format_ = 0;

    SemaphoreHandle_t frame_sem_ = nullptr;
    SemaphoreHandle_t buffer_mutex_ = nullptr;
};

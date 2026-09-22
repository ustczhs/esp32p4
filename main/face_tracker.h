#pragma once

#include <atomic>
#include <cstdint>

class FaceTracker {
public:
    static FaceTracker& GetInstance() {
        static FaceTracker instance;
        return instance;
    }

    void Start();
    void SetEnabled(bool enabled);
    void SetPreviewEnabled(bool enabled);
    bool IsEnabled() const { return enabled_.load(); }
    bool IsPreviewEnabled() const { return preview_enabled_.load(); }
    bool HasFace() const { return has_face_.load(); }
    int CenterX() const { return center_x_.load(); }
    int CenterY() const { return center_y_.load(); }
    int FrameWidth() const { return frame_w_.load(); }
    int FrameHeight() const { return frame_h_.load(); }

private:
    FaceTracker() = default;
    static void TaskEntry(void* arg);
    void RunLoop();

    std::atomic<bool> started_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<bool> preview_enabled_{true};
    std::atomic<bool> has_face_{false};
    std::atomic<int> center_x_{0};
    std::atomic<int> center_y_{0};
    std::atomic<int> frame_w_{0};
    std::atomic<int> frame_h_{0};
};

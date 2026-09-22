#ifndef CAMERA_H
#define CAMERA_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

// Lightweight view of the most recent captured frame.
// `data` is owned by the Camera until the next GrabFrame/Capture/Explain.
struct CameraFrame {
    uint8_t* data = nullptr;
    size_t len = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t format = 0;  // V4L2 FOURCC
};

class Camera {
public:
    virtual ~Camera() = default;

    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    // Grab a frame for analysis. When update_preview is false, skip LVGL preview.
    virtual bool GrabFrame(CameraFrame* out, bool update_preview) {
        (void)out;
        (void)update_preview;
        return false;
    }
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual bool SetSwapBytes(bool enabled) { return false; }  // Optional, default no-op
    virtual std::expected<std::string, std::string> Explain(const std::string& question) = 0;
};

#endif  // CAMERA_H

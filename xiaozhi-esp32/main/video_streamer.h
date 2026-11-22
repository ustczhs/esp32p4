#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class VideoStreamer {
public:
    static VideoStreamer& GetInstance() {
        static VideoStreamer instance;
        return instance;
    }

    // Call as early as possible (before AFE) to reserve INTERNAL for H.264 ref.
    void ReserveEncoderEarly();
    void Start();  // register MCP; Wi-Fi up auto-enables stream
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return enabled_.load(); }
    std::string StatusJson() const;

private:
    VideoStreamer() = default;

    static void EncodeTaskEntry(void* arg);
    void EncodeLoop();
    bool OpenEncoder(uint16_t width, uint16_t height, int bitrate);
    bool OpenBestEncoder();
    void CloseEncoder();
    bool StartHttpServer();
    void StopHttpServer();
    void PublishEncoded(const uint8_t* data, size_t len);
    static esp_err_t StreamGetHandler(httpd_req_t* req);

    static void YuyvToUyvyInPlace(uint8_t* data, size_t len);
    static void YuyvToOuYyEvYy(const uint8_t* src, int w, int h, uint8_t* dst);
    static bool ScaleYuyvNearest(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw,
                                 int dh);
    static int BitrateFor(uint16_t width, uint16_t height);

    std::atomic<bool> started_{false};
    std::atomic<bool> enabled_{false};
    std::atomic<bool> encode_running_{false};
    std::atomic<bool> force_idr_{false};
    std::atomic<bool> kick_client_{false};
    std::atomic<bool> send_busy_{false};
    std::atomic<int> clients_{0};
    std::atomic<uint32_t> frames_encoded_{0};
    std::atomic<uint32_t> bytes_sent_{0};

    httpd_handle_t httpd_ = nullptr;
    TaskHandle_t encode_task_ = nullptr;

    void* enc_ = nullptr;        // esp_h264_enc_handle_t
    void* enc_param_ = nullptr;  // esp_h264_enc_param_handle_t
    uint8_t* in_buf_ = nullptr;
    uint32_t in_buf_size_ = 0;
    uint8_t* out_buf_ = nullptr;
    uint32_t out_buf_size_ = 0;
    uint16_t enc_w_ = 0;
    uint16_t enc_h_ = 0;
    int bitrate_ = 1000000;

    SemaphoreHandle_t out_mutex_ = nullptr;
    SemaphoreHandle_t out_ready_ = nullptr;
    uint8_t* publish_buf_ = nullptr;
    size_t publish_len_ = 0;
    size_t publish_cap_ = 0;
    uint32_t publish_seq_ = 0;
    uint8_t* send_scratch_ = nullptr;
    size_t send_scratch_cap_ = 0;

    static constexpr int kPort = 8080;
    static constexpr int kFps = 10;  // lower for 2.4GHz Wi-Fi stability
    static constexpr int kGop = 10;
};

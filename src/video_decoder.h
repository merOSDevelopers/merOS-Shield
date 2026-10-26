// merOS Shield - Windows Media Foundation (WMF) Video Decoder
#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mer {

struct VideoFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t timestamp = 0; // 100-ns units
    std::vector<uint8_t> bgra; // 32-bit BGRA pixels (Direct2D compatible)
    bool valid = false;
};

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    // Initialize WMF subsystem
    static bool Startup();
    static void Shutdown();

    // Decode first or next video frame from raw MP4/H.264 byte buffer
    bool DecodeBuffer(const uint8_t* data, size_t size, VideoFrame& outFrame);

    // Decode from file path or URL
    bool DecodeFile(const std::wstring& filePath, VideoFrame& outFrame);

    // Generate YouTube-style player preview frame (dark bg, red YouTube play button, progress bar)
    static VideoFrame CreatePlayerPreview(uint32_t width = 640, uint32_t height = 360, double currentTime = 0.0, double duration = 100.0);

private:
    bool initialized_ = false;
};

} // namespace mer

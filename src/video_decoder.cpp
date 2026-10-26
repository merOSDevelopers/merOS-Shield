// merOS Shield - Windows Media Foundation (WMF) Video Decoder Implementation
#include "video_decoder.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <mferror.h>
#include <atomic>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")

#ifdef __MINGW32__
// MinGW basliklarinda eksik (Windows 7+ mfplat.dll'de mevcut)
extern "C" HRESULT WINAPI MFCreateMFByteStreamOnStream(IStream* pStream, IMFByteStream** ppByteStream);
#endif

namespace mer {

static std::atomic<int> s_wmfCount{0};
static constexpr UINT32 kMaxVideoDimension = 8192;
static constexpr size_t kMaxVideoBufferBytes = 128u * 1024 * 1024;

VideoDecoder::VideoDecoder() {
    initialized_ = Startup();
}

VideoDecoder::~VideoDecoder() {
    if (initialized_) {
        Shutdown();
        initialized_ = false;
    }
}

bool VideoDecoder::Startup() {
    if (s_wmfCount.fetch_add(1) == 0) {
        HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            s_wmfCount.fetch_sub(1);
            return false;
        }
    }
    return true;
}

void VideoDecoder::Shutdown() {
    if (s_wmfCount.fetch_sub(1) == 1) {
        MFShutdown();
    }
}

bool VideoDecoder::DecodeBuffer(const uint8_t* data, size_t size, VideoFrame& outFrame) {
    if (!data || size == 0 || size > kMaxVideoBufferBytes) return false;
    if (!initialized_) initialized_ = Startup();
    if (!initialized_) return false;

    IStream* pStream = SHCreateMemStream(data, static_cast<UINT>(size));
    if (!pStream) return false;

    IMFByteStream* pByteStream = nullptr;
    HRESULT hr = MFCreateMFByteStreamOnStream(pStream, &pByteStream);
    pStream->Release();
    if (FAILED(hr)) return false;

    IMFAttributes* pAttributes = nullptr;
    MFCreateAttributes(&pAttributes, 1);
    if (pAttributes) {
        pAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }

    IMFSourceReader* pReader = nullptr;
    hr = MFCreateSourceReaderFromByteStream(pByteStream, pAttributes, &pReader);
    if (pAttributes) pAttributes->Release();
    pByteStream->Release();
    if (FAILED(hr) || !pReader) return false;

    // Output format: Uncompressed 32-bit RGB/BGRA
    IMFMediaType* pPartialType = nullptr;
    hr = MFCreateMediaType(&pPartialType);
    if (SUCCEEDED(hr)) {
        pPartialType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pPartialType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pPartialType);
        pPartialType->Release();
    }

    DWORD streamIndex = 0;
    DWORD flags = 0;
    LONGLONG llTimeStamp = 0;
    IMFSample* pSample = nullptr;

    for (int attempts = 0; attempts < 16; ++attempts) {
        hr = pReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &llTimeStamp, &pSample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (pSample) break;
    }

    if (!pSample) {
        pReader->Release();
        return false;
    }

    UINT32 width = 0, height = 0;
    IMFMediaType* pCurrentType = nullptr;
    if (SUCCEEDED(pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrentType))) {
        MFGetAttributeSize(pCurrentType, MF_MT_FRAME_SIZE, &width, &height);
        pCurrentType->Release();
    }
    if (width == 0 || height == 0) {
        width = 640;
        height = 360;
    }
    if (width > kMaxVideoDimension || height > kMaxVideoDimension) {
        pSample->Release();
        pReader->Release();
        return false;
    }

    IMFMediaBuffer* pBuffer = nullptr;
    hr = pSample->ConvertToContiguousBuffer(&pBuffer);
    if (SUCCEEDED(hr) && pBuffer) {
        BYTE* pData = nullptr;
        DWORD maxLength = 0, currentLength = 0;
        hr = pBuffer->Lock(&pData, &maxLength, &currentLength);
        if (SUCCEEDED(hr) && pData && currentLength > 0) {
            size_t frameBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
            if (frameBytes <= kMaxVideoBufferBytes) {
                outFrame.width = width;
                outFrame.height = height;
                outFrame.timestamp = static_cast<uint64_t>(llTimeStamp);
                outFrame.bgra.resize(frameBytes);

                size_t copyLen = (std::min)(static_cast<size_t>(currentLength), outFrame.bgra.size());
                memcpy(outFrame.bgra.data(), pData, copyLen);
                outFrame.valid = true;
            }
            pBuffer->Unlock();
        }
        pBuffer->Release();
    }

    pSample->Release();
    pReader->Release();
    return outFrame.valid;
}

bool VideoDecoder::DecodeFile(const std::wstring& filePath, VideoFrame& outFrame) {
    if (filePath.empty()) return false;
    if (!initialized_) initialized_ = Startup();
    if (!initialized_) return false;

    IMFAttributes* pAttributes = nullptr;
    MFCreateAttributes(&pAttributes, 1);
    if (pAttributes) {
        pAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }

    IMFSourceReader* pReader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(filePath.c_str(), pAttributes, &pReader);
    if (pAttributes) pAttributes->Release();
    if (FAILED(hr) || !pReader) return false;

    IMFMediaType* pPartialType = nullptr;
    if (SUCCEEDED(MFCreateMediaType(&pPartialType))) {
        pPartialType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pPartialType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pPartialType);
        pPartialType->Release();
    }

    DWORD streamIndex = 0;
    DWORD flags = 0;
    LONGLONG llTimeStamp = 0;
    IMFSample* pSample = nullptr;

    for (int attempts = 0; attempts < 16; ++attempts) {
        hr = pReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &llTimeStamp, &pSample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (pSample) break;
    }

    if (!pSample) {
        pReader->Release();
        return false;
    }

    UINT32 width = 0, height = 0;
    IMFMediaType* pCurrentType = nullptr;
    if (SUCCEEDED(pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrentType))) {
        MFGetAttributeSize(pCurrentType, MF_MT_FRAME_SIZE, &width, &height);
        pCurrentType->Release();
    }
    if (width == 0 || height == 0) { width = 640; height = 360; }
    if (width > kMaxVideoDimension || height > kMaxVideoDimension) {
        pSample->Release();
        pReader->Release();
        return false;
    }

    IMFMediaBuffer* pBuffer = nullptr;
    hr = pSample->ConvertToContiguousBuffer(&pBuffer);
    if (SUCCEEDED(hr) && pBuffer) {
        BYTE* pData = nullptr;
        DWORD maxLength = 0, currentLength = 0;
        hr = pBuffer->Lock(&pData, &maxLength, &currentLength);
        if (SUCCEEDED(hr) && pData && currentLength > 0) {
            size_t frameBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
            if (frameBytes <= kMaxVideoBufferBytes) {
                outFrame.width = width;
                outFrame.height = height;
                outFrame.timestamp = static_cast<uint64_t>(llTimeStamp);
                outFrame.bgra.resize(frameBytes);
                size_t copyLen = (std::min)(static_cast<size_t>(currentLength), outFrame.bgra.size());
                memcpy(outFrame.bgra.data(), pData, copyLen);
                outFrame.valid = true;
            }
            pBuffer->Unlock();
        }
        pBuffer->Release();
    }

    pSample->Release();
    pReader->Release();
    return outFrame.valid;
}

VideoFrame VideoDecoder::CreatePlayerPreview(uint32_t width, uint32_t height, double currentTime, double duration) {
    VideoFrame frame;
    frame.width = (width > 0 && width <= kMaxVideoDimension) ? width : 640;
    frame.height = (height > 0 && height <= kMaxVideoDimension) ? height : 360;
    frame.valid = true;
    frame.bgra.resize(static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height) * 4u);

    uint8_t* p = frame.bgra.data();
    // 1. Dark YouTube video canvas (#0f0f0f)
    for (uint32_t y = 0; y < frame.height; ++y) {
        for (uint32_t x = 0; x < frame.width; ++x) {
            size_t idx = (static_cast<size_t>(y) * frame.width + x) * 4;
            p[idx + 0] = 18; // B
            p[idx + 1] = 18; // G
            p[idx + 2] = 18; // R
            p[idx + 3] = 255; // A
        }
    }

    // 2. YouTube red play button in center
    int32_t btnW = 68;
    int32_t btnH = 48;
    int32_t btnX = (static_cast<int32_t>(frame.width) - btnW) / 2;
    int32_t btnY = (static_cast<int32_t>(frame.height) - btnH) / 2;

    for (int32_t y = btnY; y < btnY + btnH; ++y) {
        if (y < 0 || y >= static_cast<int32_t>(frame.height)) continue;
        for (int32_t x = btnX; x < btnX + btnW; ++x) {
            if (x < 0 || x >= static_cast<int32_t>(frame.width)) continue;
            // Corner rounding
            int dx = (x < btnX + 8) ? (btnX + 8 - x) : (x > btnX + btnW - 8) ? (x - (btnX + btnW - 8)) : 0;
            int dy = (y < btnY + 8) ? (btnY + 8 - y) : (y > btnY + btnH - 8) ? (y - (btnY + btnH - 8)) : 0;
            if (dx * dx + dy * dy > 64) continue;

            size_t idx = (static_cast<size_t>(y) * frame.width + static_cast<size_t>(x)) * 4;
            // YouTube Red #FF0000
            p[idx + 0] = 0;
            p[idx + 1] = 0;
            p[idx + 2] = 255;
            p[idx + 3] = 255;
        }
    }

    // 3. White Play triangle inside red button
    int32_t triX = btnX + 26;
    int32_t triY = btnY + 14;
    int32_t triH = 20;
    for (int32_t ty = 0; ty < triH; ++ty) {
        int32_t rowY = triY + ty;
        if (rowY < 0 || rowY >= static_cast<int32_t>(frame.height)) continue;
        int32_t span = (ty <= triH / 2) ? (ty * 18 / (triH / 2)) : ((triH - ty) * 18 / (triH / 2));
        for (int32_t tx = 0; tx < span; ++tx) {
            int32_t colX = triX + tx;
            if (colX < 0 || colX >= static_cast<int32_t>(frame.width)) continue;
            size_t idx = (static_cast<size_t>(rowY) * frame.width + static_cast<size_t>(colX)) * 4;
            p[idx + 0] = 255; // White
            p[idx + 1] = 255;
            p[idx + 2] = 255;
            p[idx + 3] = 255;
        }
    }

    // 4. Bottom progress bar
    int32_t barY = static_cast<int32_t>(frame.height) - 10;
    int32_t barH = 4;
    double progressRatio = (duration > 0.0) ? (currentTime / duration) : 0.0;
    if (progressRatio < 0.0) progressRatio = 0.0;
    if (progressRatio > 1.0) progressRatio = 1.0;
    int32_t playedW = static_cast<int32_t>(progressRatio * frame.width);

    for (int32_t by = std::max<int32_t>(0, barY); by < barY + barH && by < static_cast<int32_t>(frame.height); ++by) {
        for (uint32_t bx = 0; bx < frame.width; ++bx) {
            size_t idx = (static_cast<size_t>(by) * frame.width + bx) * 4;
            if (static_cast<int32_t>(bx) <= playedW) {
                // Red progress
                p[idx + 0] = 0;
                p[idx + 1] = 0;
                p[idx + 2] = 255;
            } else {
                // Gray track
                p[idx + 0] = 80;
                p[idx + 1] = 80;
                p[idx + 2] = 80;
            }
            p[idx + 3] = 255;
        }
    }

    return frame;
}

} // namespace mer

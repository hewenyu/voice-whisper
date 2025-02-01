#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <thread>
#include <atomic>
#include <iostream>
#include <queue>
#include <mutex>
#include <vector>
#include <memory>
#include "../audio_capture/windows/wasapi_capture.h"

// 定义常量
const REFERENCE_TIME REFTIMES_PER_SEC = 10000000;
const REFERENCE_TIME REFTIMES_PER_MILLISEC = 10000;
const int SAMPLE_RATE = 16000;
const int BITS_PER_SAMPLE = 16;
const int BLOCK_ALIGN = 2;

// 音频缓冲队列
class AudioBuffer {
public:
    std::vector<float> data;
    size_t size;

    AudioBuffer() : size(0) {}
    AudioBuffer(const float* ptr, size_t count) : data(ptr, ptr + count), size(count) {}
};

class VirtualAudioDevice {
private:
    std::queue<AudioBuffer> buffer_queue_;
    std::mutex queue_mutex_;
    std::atomic<bool> running_;
    std::thread render_thread_;
    IAudioClient* audio_client_;
    IAudioRenderClient* render_client_;
    HANDLE audio_event_;
    HANDLE render_thread_handle_;
    DWORD task_index_;
    void* wasapi_capture_;
    WAVEFORMATEX wave_format_;

    static void CALLBACK audio_callback(void* user_data, float* data, int frame_count) {
        auto* device = static_cast<VirtualAudioDevice*>(user_data);
        device->queue_audio_data(data, frame_count);
    }

    void queue_audio_data(float* data, int frame_count) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        AudioBuffer buffer;
        buffer.data.assign(data, data + frame_count);
        buffer.size = frame_count;
        buffer_queue_.push(buffer);
    }

    void render_proc() {
        HANDLE wait_array[1] = { audio_event_ };

        while (running_) {
            DWORD wait_result = WaitForMultipleObjects(1, wait_array, FALSE, INFINITE);
            if (wait_result != WAIT_OBJECT_0) break;

            UINT32 padding = 0;
            HRESULT hr = audio_client_->GetCurrentPadding(&padding);
            if (FAILED(hr)) break;

            UINT32 buffer_size = wave_format_.nSamplesPerSec / 100;  // 10ms buffer
            UINT32 available_frames = buffer_size - padding;
            if (available_frames == 0) continue;

            BYTE* buffer_data = nullptr;
            hr = render_client_->GetBuffer(available_frames, &buffer_data);
            if (FAILED(hr)) break;

            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!buffer_queue_.empty()) {
                AudioBuffer& audio_buffer = buffer_queue_.front();
                size_t frames_to_copy = std::min(static_cast<size_t>(available_frames), audio_buffer.size);
                
                float* dest_buffer = reinterpret_cast<float*>(buffer_data);
                memcpy(dest_buffer, audio_buffer.data.data(), frames_to_copy * sizeof(float));
                
                if (frames_to_copy < audio_buffer.size) {
                    audio_buffer.data.erase(audio_buffer.data.begin(), audio_buffer.data.begin() + frames_to_copy);
                    audio_buffer.size -= frames_to_copy;
                } else {
                    buffer_queue_.pop();
                }
                
                render_client_->ReleaseBuffer(frames_to_copy, 0);
            } else {
                render_client_->ReleaseBuffer(available_frames, AUDCLNT_BUFFERFLAGS_SILENT);
            }
        }
    }

    bool setup_audio_client() {
        // 设置音频格式
        wave_format_.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wave_format_.nChannels = 1;  // 单声道
        wave_format_.nSamplesPerSec = SAMPLE_RATE;
        wave_format_.wBitsPerSample = 32;  // float格式
        wave_format_.nBlockAlign = (wave_format_.nChannels * wave_format_.wBitsPerSample) / 8;
        wave_format_.nAvgBytesPerSec = wave_format_.nSamplesPerSec * wave_format_.nBlockAlign;
        wave_format_.cbSize = 0;

        // 创建音频客户端
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        HRESULT hr;

        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
            (void**)&enumerator
        );
        if (FAILED(hr)) return false;

        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) {
            enumerator->Release();
            return false;
        }

        hr = device->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL,
            nullptr, (void**)&audio_client_
        );

        device->Release();
        enumerator->Release();

        if (FAILED(hr)) return false;

        // 初始化音频客户端
        hr = audio_client_->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            REFTIMES_PER_SEC / 100,  // 10ms buffer
            0,
            &wave_format_,
            nullptr
        );
        if (FAILED(hr)) return false;

        // 获取渲染客户端
        hr = audio_client_->GetService(
            __uuidof(IAudioRenderClient),
            (void**)&render_client_
        );
        if (FAILED(hr)) return false;

        // 设置事件
        hr = audio_client_->SetEventHandle(audio_event_);
        if (FAILED(hr)) return false;

        return true;
    }

public:
    VirtualAudioDevice() : running_(false), audio_client_(nullptr), render_client_(nullptr),
                          audio_event_(nullptr), render_thread_handle_(nullptr), task_index_(0) {
        wasapi_capture_ = wasapi_capture_create();
    }

    ~VirtualAudioDevice() {
        stop();
        if (wasapi_capture_) {
            wasapi_capture_destroy(wasapi_capture_);
        }
    }

    bool initialize() {
        // 初始化WASAPI捕获
        if (!wasapi_capture_initialize(wasapi_capture_)) {
            std::cerr << "Failed to initialize WASAPI capture" << std::endl;
            return false;
        }

        // 设置音频回调
        wasapi_capture_set_callback(wasapi_capture_, audio_callback, this);

        // 创建事件
        audio_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!audio_event_) return false;

        // 设置音频客户端
        if (!setup_audio_client()) {
            std::cerr << "Failed to setup audio client" << std::endl;
            return false;
        }

        return true;
    }

    bool start() {
        if (running_) return true;

        // 启动音频客户端
        HRESULT hr = audio_client_->Start();
        if (FAILED(hr)) return false;

        // 启动WASAPI捕获
        if (!wasapi_capture_start(wasapi_capture_)) {
            audio_client_->Stop();
            std::cerr << "Failed to start WASAPI capture" << std::endl;
            return false;
        }

        running_ = true;
        render_thread_ = std::thread(&VirtualAudioDevice::render_proc, this);

        return true;
    }

    void stop() {
        if (!running_) return;

        running_ = false;
        if (render_thread_.joinable()) {
            render_thread_.join();
        }

        wasapi_capture_stop(wasapi_capture_);

        if (render_client_) {
            render_client_->Release();
            render_client_ = nullptr;
        }

        if (audio_client_) {
            audio_client_->Stop();
            audio_client_->Release();
            audio_client_ = nullptr;
        }

        if (audio_event_) {
            CloseHandle(audio_event_);
            audio_event_ = nullptr;
        }
    }
};

int main() {
    // 初始化COM
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize COM" << std::endl;
        return 1;
    }

    VirtualAudioDevice device;
    if (!device.initialize()) {
        std::cerr << "Failed to initialize virtual audio device" << std::endl;
        CoUninitialize();
        return 1;
    }

    if (!device.start()) {
        std::cerr << "Failed to start virtual audio device" << std::endl;
        CoUninitialize();
        return 1;
    }

    std::cout << "Virtual audio device is running. Press Enter to stop..." << std::endl;
    std::cin.get();

    device.stop();
    CoUninitialize();
    return 0;
}

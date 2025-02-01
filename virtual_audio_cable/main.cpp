#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <avrt.h>
#include <thread>
#include <atomic>
#include <iostream>
#include <queue>
#include <mutex>
#include <vector>
#include <memory>
#include <sstream>
#include <string>
#include <Functiondiscoverykeys_devpkey.h>
#include "policy_config.h"
#include "../audio_capture/windows/wasapi_capture.h"

// 只定义虚拟设备GUID
DEFINE_GUID(VIRTUAL_AUDIO_GUID, 0x12345678, 0x1234, 0x1234, 0x12, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x34);

// 定义常量
const REFERENCE_TIME REFTIMES_PER_SEC = 10000000;
const REFERENCE_TIME REFTIMES_PER_MILLISEC = 10000;
const int SAMPLE_RATE = 16000;
const int BITS_PER_SAMPLE = 16;
const int BLOCK_ALIGN = 2;

// 辅助函数：将HRESULT转换为错误信息
std::string GetErrorMessage(HRESULT hr) {
    char* message = nullptr;
    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        hr,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&message),
        0,
        nullptr
    );
    std::string result = message ? message : "Unknown error";
    LocalFree(message);
    return result;
}

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
    IAudioCaptureClient* capture_client_;
    IAudioRenderClient* render_client_;
    HANDLE audio_event_;
    HANDLE render_thread_handle_;
    DWORD task_index_;
    void* wasapi_capture_;
    WAVEFORMATEX wave_format_;
    DWORD target_process_id_;
    IPolicyConfig* policy_config_;
    std::wstring device_id_;

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
        const DWORD sleep_time = 10; // 10ms sleep between polls

        while (running_) {
            UINT32 padding = 0;
            HRESULT hr = audio_client_->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                std::cerr << "Failed to get current padding: " << GetErrorMessage(hr) << std::endl;
                break;
            }

            BYTE* data = nullptr;
            UINT32 num_frames_to_read = 0;
            DWORD flags = 0;
            UINT64 position = 0;
            UINT64 qpc_position = 0;

            hr = capture_client_->GetBuffer(
                &data,
                &num_frames_to_read,
                &flags,
                &position,
                &qpc_position
            );

            if (FAILED(hr)) {
                if (hr != AUDCLNT_S_BUFFER_EMPTY) {
                    std::cerr << "Failed to get capture buffer: " << GetErrorMessage(hr) << std::endl;
                    break;
                }
                Sleep(sleep_time);
                continue;
            }

            if (num_frames_to_read == 0) {
                Sleep(sleep_time);
                continue;
            }

            // 处理音频数据
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                AudioBuffer buffer(reinterpret_cast<float*>(data), num_frames_to_read);
                buffer_queue_.push(buffer);
            }

            // 释放缓冲区
            hr = capture_client_->ReleaseBuffer(num_frames_to_read);
            if (FAILED(hr)) {
                std::cerr << "Failed to release capture buffer: " << GetErrorMessage(hr) << std::endl;
                break;
            }
        }
    }

    bool setup_audio_client() {
        // 创建音频客户端
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        HRESULT hr;

        std::cout << "Creating device enumerator..." << std::endl;
        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
            (void**)&enumerator
        );
        if (FAILED(hr)) {
            std::cerr << "Failed to create device enumerator: " << GetErrorMessage(hr) << std::endl;
            return false;
        }

        // 获取默认音频输出设备
        std::cout << "Getting default audio endpoint..." << std::endl;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) {
            std::cerr << "Failed to get default audio endpoint: " << GetErrorMessage(hr) << std::endl;
            enumerator->Release();
            return false;
        }

        std::cout << "Activating audio client..." << std::endl;
        hr = device->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL,
            nullptr, (void**)&audio_client_
        );

        device->Release();
        enumerator->Release();

        if (FAILED(hr)) {
            std::cerr << "Failed to activate audio client: " << GetErrorMessage(hr) << std::endl;
            return false;
        }

        // 获取当前音频格式
        WAVEFORMATEX* device_format = nullptr;
        hr = audio_client_->GetMixFormat(&device_format);
        if (FAILED(hr)) {
            std::cerr << "Failed to get mix format: " << GetErrorMessage(hr) << std::endl;
            return false;
        }

        // 打印原始格式信息
        std::cout << "Original format:" << std::endl;
        std::cout << "  Sample rate: " << device_format->nSamplesPerSec << std::endl;
        std::cout << "  Channels: " << device_format->nChannels << std::endl;
        std::cout << "  Bits per sample: " << device_format->wBitsPerSample << std::endl;
        std::cout << "  Format tag: 0x" << std::hex << device_format->wFormatTag << std::dec << std::endl;

        // 检查是否是WAVE_FORMAT_EXTENSIBLE格式
        if (device_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            WAVEFORMATEXTENSIBLE* format_ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(device_format);
            std::cout << "  Sub format: " << std::hex 
                      << format_ext->SubFormat.Data1 << "-"
                      << format_ext->SubFormat.Data2 << "-"
                      << format_ext->SubFormat.Data3 << std::dec << std::endl;
        }

        // 使用设备的原生格式
        size_t format_size = sizeof(WAVEFORMATEX);
        if (device_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            format_size = sizeof(WAVEFORMATEXTENSIBLE);
        }
        memcpy(&wave_format_, device_format, format_size);
        
        CoTaskMemFree(device_format);

        // 计算缓冲区持续时间
        REFERENCE_TIME default_period = 0, min_period = 0;
        hr = audio_client_->GetDevicePeriod(&default_period, &min_period);
        if (FAILED(hr)) {
            std::cerr << "Failed to get device period: " << GetErrorMessage(hr) << std::endl;
            return false;
        }

        std::cout << "Initializing audio client..." << std::endl;
        
        // 使用更简单的标志组合
        const DWORD stream_flags = AUDCLNT_STREAMFLAGS_LOOPBACK;
        const REFERENCE_TIME buffer_duration = default_period * 2;  // 使用两倍的默认周期

        hr = audio_client_->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            stream_flags,
            buffer_duration,
            0,
            &wave_format_,
            nullptr
        );

        if (FAILED(hr)) {
            std::cerr << "Failed to initialize audio client with error: " << GetErrorMessage(hr) 
                      << " (0x" << std::hex << hr << ")" << std::endl;
            std::cerr << "Attempted initialization with:" << std::endl;
            std::cerr << "  Share mode: AUDCLNT_SHAREMODE_SHARED" << std::endl;
            std::cerr << "  Stream flags: 0x" << std::hex << stream_flags << std::dec << std::endl;
            std::cerr << "  Buffer duration: " << buffer_duration << " (100ns units)" << std::endl;
            std::cerr << "  Wave format:" << std::endl;
            std::cerr << "    Format tag: 0x" << std::hex << wave_format_.wFormatTag << std::dec << std::endl;
            std::cerr << "    Channels: " << wave_format_.nChannels << std::endl;
            std::cerr << "    Sample rate: " << wave_format_.nSamplesPerSec << std::endl;
            std::cerr << "    Bits per sample: " << wave_format_.wBitsPerSample << std::endl;
            std::cerr << "    Block align: " << wave_format_.nBlockAlign << std::endl;
            std::cerr << "    Avg bytes per sec: " << wave_format_.nAvgBytesPerSec << std::endl;
            return false;
        }

        // 获取实际的缓冲区大小
        UINT32 buffer_size;
        hr = audio_client_->GetBufferSize(&buffer_size);
        if (FAILED(hr)) {
            std::cerr << "Failed to get buffer size: " << GetErrorMessage(hr) << std::endl;
            return false;
        }

        std::cout << "Getting capture client..." << std::endl;
        hr = audio_client_->GetService(
            __uuidof(IAudioCaptureClient),
            (void**)&capture_client_
        );
        if (FAILED(hr)) {
            std::cerr << "Failed to get capture client: " << GetErrorMessage(hr) << std::endl;
            return false;
        }

        std::cout << "Audio configuration:" << std::endl;
        std::cout << "  Format: " << wave_format_.nSamplesPerSec << "Hz, "
                  << wave_format_.nChannels << " channels, "
                  << wave_format_.wBitsPerSample << " bits" << std::endl;
        std::cout << "  Default period: " << default_period << " (100ns units)" << std::endl;
        std::cout << "  Minimum period: " << min_period << " (100ns units)" << std::endl;
        std::cout << "  Buffer size: " << buffer_size << " frames" << std::endl;
        std::cout << "  Buffer duration: " << buffer_duration << " (100ns units)" << std::endl;

        std::cout << "Audio client setup completed successfully" << std::endl;
        return true;
    }

    bool list_applications() {
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioSessionManager2* session_manager = nullptr;
        HRESULT hr;

        // 创建设备枚举器
        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
            (void**)&enumerator
        );
        if (FAILED(hr)) {
            std::cerr << "Failed to create device enumerator: " << GetErrorMessage(hr) << std::endl;
            return false;
        }

        // 获取默认音频输出设备
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) {
            std::cerr << "Failed to get default audio endpoint" << std::endl;
            enumerator->Release();
            return false;
        }

        // 获取会话管理器
        hr = device->Activate(
            __uuidof(IAudioSessionManager2),
            CLSCTX_ALL,
            nullptr,
            (void**)&session_manager
        );

        device->Release();
        enumerator->Release();

        if (FAILED(hr)) {
            std::cerr << "Failed to get audio session manager" << std::endl;
            return false;
        }

        // 获取会话枚举器
        IAudioSessionEnumerator* sessionEnumerator = nullptr;
        hr = session_manager->GetSessionEnumerator(&sessionEnumerator);
        if (FAILED(hr)) {
            std::cerr << "Failed to get session enumerator" << std::endl;
            session_manager->Release();
            return false;
        }

        // 获取会话数量
        int sessionCount;
        hr = sessionEnumerator->GetCount(&sessionCount);
        if (FAILED(hr)) {
            std::cerr << "Failed to get session count" << std::endl;
            sessionEnumerator->Release();
            session_manager->Release();
            return false;
        }

        std::cout << "Available audio sessions:" << std::endl;
        std::cout << "PID\tProcess Name" << std::endl;
        std::cout << "------------------------" << std::endl;

        // 遍历所有会话
        for (int i = 0; i < sessionCount; i++) {
            IAudioSessionControl* sessionControl = nullptr;
            hr = sessionEnumerator->GetSession(i, &sessionControl);
            if (FAILED(hr)) continue;

            IAudioSessionControl2* sessionControl2 = nullptr;
            hr = sessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&sessionControl2);
            sessionControl->Release();
            if (FAILED(hr)) continue;

            // 获取进程ID
            DWORD processId;
            hr = sessionControl2->GetProcessId(&processId);
            if (SUCCEEDED(hr) && processId != 0) {
                HANDLE processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
                if (processHandle) {
                    wchar_t processName[MAX_PATH];
                    DWORD size = MAX_PATH;
                    if (QueryFullProcessImageNameW(processHandle, 0, processName, &size)) {
                        // 获取文件名部分
                        wchar_t* fileName = wcsrchr(processName, L'\\');
                        fileName = fileName ? fileName + 1 : processName;
                        std::wcout << processId << L"\t" << fileName << std::endl;
                    }
                    CloseHandle(processHandle);
                }
            }
            sessionControl2->Release();
        }

        sessionEnumerator->Release();
        session_manager->Release();
        return true;
    }

    bool register_endpoint() {
        std::cout << "Creating policy config instance..." << std::endl;
        HRESULT hr = CoCreateInstance(
            CLSID_CPolicyConfigClient,
            nullptr,
            CLSCTX_ALL,
            IID_IPolicyConfig,
            (void**)&policy_config_
        );

        if (FAILED(hr)) {
            std::cerr << "Failed to create policy config: " << GetErrorMessage(hr) << std::endl;
            std::cerr << "Error code: 0x" << std::hex << hr << std::dec << std::endl;
            std::cerr << "This may be due to insufficient privileges. Please run as administrator." << std::endl;
            return false;
        }

        // 创建唯一的设备ID
        std::cout << "Creating virtual device ID..." << std::endl;
        wchar_t guid_str[39];
        StringFromGUID2(VIRTUAL_AUDIO_GUID, guid_str, 39);
        device_id_ = L"SWD\\MMDEVAPI\\";
        device_id_ += guid_str;

        std::wcout << L"Registering virtual device with ID: " << device_id_ << std::endl;
        
        // 注册虚拟设备
        std::cout << "Calling RegisterAudioEndpoint..." << std::endl;
        GUID moduleId = VIRTUAL_AUDIO_GUID;  // 创建一个非const副本
        hr = policy_config_->RegisterAudioEndpoint(
            device_id_.c_str(),
            L"Virtual Audio Device",
            L"Virtual",
            eRender,  // 改为eRender，因为我们要创建一个播放设备
            DEVICE_STATE_ACTIVE,
            &moduleId
        );

        if (FAILED(hr)) {
            std::cerr << "Failed to register audio endpoint: " << GetErrorMessage(hr) << std::endl;
            std::cerr << "Error code: 0x" << std::hex << hr << std::dec << std::endl;
            
            // 检查常见错误
            if (hr == E_ACCESSDENIED) {
                std::cerr << "Access denied. Please ensure you are running as administrator." << std::endl;
            }
            else if (hr == REGDB_E_CLASSNOTREG) {
                std::cerr << "Class not registered. The PolicyConfig interface may not be available on this system." << std::endl;
            }
            else if (hr == E_INVALIDARG) {
                std::cerr << "Invalid argument passed to RegisterAudioEndpoint." << std::endl;
            }
            
            return false;
        }

        std::cout << "Virtual audio endpoint registered successfully" << std::endl;
        
        // 验证设备是否真的被创建
        std::cout << "Verifying device creation..." << std::endl;
        IMMDeviceEnumerator* enumerator = nullptr;
        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
            (void**)&enumerator
        );
        
        if (SUCCEEDED(hr)) {
            IMMDevice* device = nullptr;
            hr = enumerator->GetDevice(device_id_.c_str(), &device);
            
            if (SUCCEEDED(hr)) {
                std::cout << "Device verification successful" << std::endl;
                device->Release();
            } else {
                std::cerr << "Device verification failed: " << GetErrorMessage(hr) << std::endl;
            }
            
            enumerator->Release();
        }

        return true;
    }

    bool unregister_endpoint() {
        if (policy_config_ && !device_id_.empty()) {
            HRESULT hr = policy_config_->UnregisterAudioEndpoint(device_id_.c_str());
            if (FAILED(hr)) {
                std::cerr << "Failed to unregister audio endpoint: " << GetErrorMessage(hr) << std::endl;
                return false;
            }
        }
        return true;
    }

public:
    VirtualAudioDevice() : running_(false), audio_client_(nullptr), capture_client_(nullptr),
                          render_client_(nullptr), audio_event_(nullptr), 
                          render_thread_handle_(nullptr), task_index_(0), target_process_id_(0),
                          policy_config_(nullptr) {
        wasapi_capture_ = wasapi_capture_create();
    }

    ~VirtualAudioDevice() {
        stop();
        if (wasapi_capture_) {
            wasapi_capture_destroy(wasapi_capture_);
        }
        if (policy_config_) {
            unregister_endpoint();
            policy_config_->Release();
        }
    }

    bool initialize(DWORD target_pid = 0) {
        std::cout << "Starting device initialization..." << std::endl;
        
        // 注册虚拟设备端点
        if (!register_endpoint()) {
            std::cerr << "Failed to register virtual audio device" << std::endl;
            return false;
        }

        std::cout << "Initializing WASAPI capture..." << std::endl;
        // 初始化WASAPI捕获
        if (!wasapi_capture_initialize(wasapi_capture_)) {
            std::cerr << "Failed to initialize WASAPI capture" << std::endl;
            return false;
        }

        std::cout << "Setting up audio callback..." << std::endl;
        // 设置音频回调
        wasapi_capture_set_callback(wasapi_capture_, audio_callback, this);

        std::cout << "Creating audio event..." << std::endl;
        // 创建事件
        audio_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!audio_event_) {
            std::cerr << "Failed to create audio event" << std::endl;
            return false;
        }

        std::cout << "Setting up audio client..." << std::endl;
        // 设置音频客户端
        if (!setup_audio_client()) {
            std::cerr << "Failed to setup audio client" << std::endl;
            return false;
        }

        // 保存目标PID，在start时使用
        target_process_id_ = target_pid;
        std::cout << "Initialization completed successfully" << std::endl;

        return true;
    }

    bool list_apps() {
        return list_applications();
    }

    bool start() {
        if (running_) return true;

        // 启动音频客户端
        HRESULT hr = audio_client_->Start();
        if (FAILED(hr)) {
            std::cerr << "Failed to start audio client: " << GetErrorMessage(hr) << std::endl;
            return false;
        }

        // 如果有指定的目标PID，先设置进程捕获
        if (target_process_id_ != 0) {
            std::cout << "Starting capture for process " << target_process_id_ << "..." << std::endl;
            if (!wasapi_capture_start_process(wasapi_capture_, target_process_id_)) {
                std::cerr << "Failed to start capturing specific process" << std::endl;
                audio_client_->Stop();
                return false;
            }
        } else {
            // 启动普通WASAPI捕获
            if (!wasapi_capture_start(wasapi_capture_)) {
                std::cerr << "Failed to start WASAPI capture" << std::endl;
                audio_client_->Stop();
                return false;
            }
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

        if (capture_client_) {
            capture_client_->Release();
            capture_client_ = nullptr;
        }

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

void print_usage() {
    std::cout << "Usage:" << std::endl;
    std::cout << "  virtual_audio_cable [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --list    List all applications with audio sessions" << std::endl;
    std::cout << "  -p <pid>  Capture audio from specific process ID" << std::endl;
}

int main(int argc, char* argv[]) {
    // 初始化COM
    std::cout << "Initializing COM..." << std::endl;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize COM: " << GetErrorMessage(hr) << std::endl;
        return 1;
    }

    std::cout << "Creating virtual audio device..." << std::endl;
    VirtualAudioDevice device;

    // 解析命令行参数
    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "--list") {
            std::cout << "Listing audio applications..." << std::endl;
            bool result = device.list_apps();
            CoUninitialize();
            return result ? 0 : 1;
        }
        else if (arg == "-p" && argc > 2) {
            DWORD pid = static_cast<DWORD>(std::stoul(argv[2]));
            std::cout << "Initializing virtual audio device for PID " << pid << "..." << std::endl;
            if (!device.initialize(pid)) {
                std::cerr << "Failed to initialize virtual audio device for PID " << pid << std::endl;
                CoUninitialize();
                return 1;
            }
        }
        else {
            print_usage();
            CoUninitialize();
            return 1;
        }
    }
    else {
        std::cout << "Initializing virtual audio device..." << std::endl;
        if (!device.initialize()) {
            std::cerr << "Failed to initialize virtual audio device" << std::endl;
            CoUninitialize();
            return 1;
        }
    }

    std::cout << "Starting virtual audio device..." << std::endl;
    if (!device.start()) {
        std::cerr << "Failed to start virtual audio device" << std::endl;
        CoUninitialize();
        return 1;
    }

    std::cout << "Virtual audio device is running. Press Enter to stop..." << std::endl;
    std::cin.get();

    std::cout << "Stopping virtual audio device..." << std::endl;
    device.stop();
    
    std::cout << "Cleaning up..." << std::endl;
    CoUninitialize();
    return 0;
}

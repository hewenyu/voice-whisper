#ifndef WASAPI_CAPTURE_H
#define WASAPI_CAPTURE_H

#include <windows.h>
#include <audiopolicy.h>
#include <vector>
#include <mutex>
#include <atomic>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*audio_callback)(void* user_data, float* buffer, int frames);

// 声明Go回调函数
extern void goAudioCallback(void* user_data, float* buffer, int frames);

// 应用程序信息结构体
typedef struct {
    unsigned int pid;
    wchar_t name[260]; // 使用 260 替代 MAX_PATH
} AudioAppInfo;

// 音频格式结构体
typedef struct {
    unsigned int sample_rate;
    unsigned int channels;
    unsigned int bits_per_sample;
} AudioFormat;

void* wasapi_capture_create();
void wasapi_capture_destroy(void* handle);
int wasapi_capture_initialize(void* handle);
int wasapi_capture_start(void* handle);
void wasapi_capture_stop(void* handle);
void wasapi_capture_set_callback(void* handle, audio_callback callback, void* user_data);

// 新增：获取应用程序列表
int wasapi_capture_get_applications(void* handle, AudioAppInfo* apps, int max_count);

// 在现有函数声明后添加
int wasapi_capture_start_process(void* handle, unsigned int pid);

// 添加获取格式的函数声明
int wasapi_capture_get_format(void* handle, AudioFormat* format);

#ifdef __cplusplus
}

// SDL-style audio capture class that wraps WASAPI functionality
class audio_async_wasapi {
public:
    audio_async_wasapi(int len_ms);
    ~audio_async_wasapi();

    bool init(int capture_id = -1, int sample_rate = 16000);
    bool resume();
    bool pause();
    bool clear();
    void get(int ms, std::vector<float>& audio);

private:
    static void audio_callback_wrapper(void* user_data, float* buffer, int frames);
    
    void* wasapi_handle_;
    int len_ms_;
    int sample_rate_;
    std::atomic_bool running_;
    std::mutex mutex_;
    std::vector<float> audio_buffer_;
    size_t buffer_pos_;
    size_t buffer_len_;
};

#endif

#endif // WASAPI_CAPTURE_H 
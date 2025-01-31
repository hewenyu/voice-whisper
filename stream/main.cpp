#include "whisper.h"
#include "../audio_capture/windows/wasapi_capture.h"
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <queue>

// 音频缓冲队列
struct AudioBuffer {
    std::mutex mtx;
    std::queue<std::vector<float>> queue;
    static const int MAX_SIZE = 100;  // 最大缓冲数量
};

// 全局变量
AudioBuffer g_audio_buffer;
std::atomic<bool> g_is_running{true};

// 音频回调函数 - 使用static避免命名冲突
static void audio_data_callback(void* user_data, float* buffer, int frames) {
    auto& audio_buffer = *static_cast<AudioBuffer*>(user_data);
    std::vector<float> frame_data(buffer, buffer + frames);
    
    std::lock_guard<std::mutex> lock(audio_buffer.mtx);
    if (audio_buffer.queue.size() < AudioBuffer::MAX_SIZE) {
        audio_buffer.queue.push(std::move(frame_data));
    }
}

// whisper处理线程
void whisper_processing_thread(struct whisper_context* ctx) {
    // whisper参数设置
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_progress = false;
    wparams.print_special = false;
    wparams.print_realtime = true;
    wparams.print_timestamps = true;
    wparams.translate = false;
    wparams.single_segment = true;
    wparams.max_tokens = 32;
    wparams.language = "zh";
    wparams.n_threads = 4;

    std::vector<float> audio_data;
    const int n_samples_30s = 30 * WHISPER_SAMPLE_RATE;
    audio_data.reserve(n_samples_30s);

    while (g_is_running) {
        {
            std::lock_guard<std::mutex> lock(g_audio_buffer.mtx);
            while (!g_audio_buffer.queue.empty()) {
                auto& chunk = g_audio_buffer.queue.front();
                audio_data.insert(audio_data.end(), chunk.begin(), chunk.end());
                g_audio_buffer.queue.pop();
            }
        }

        // 当累积足够的音频数据时进行处理
        if (audio_data.size() >= WHISPER_SAMPLE_RATE * 3) {  // 处理3秒的音频
            if (whisper_full(ctx, wparams, audio_data.data(), audio_data.size()) != 0) {
                fprintf(stderr, "Failed to process audio\n");
                continue;
            }

            // 输出识别结果
            const int n_segments = whisper_full_n_segments(ctx);
            for (int i = 0; i < n_segments; ++i) {
                const char* text = whisper_full_get_segment_text(ctx, i);
                printf("%s", text);
                fflush(stdout);
            }
            printf("\n");

            // 清空处理过的音频数据
            audio_data.clear();
            audio_data.reserve(n_samples_30s);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <model_path>\n", argv[0]);
        return 1;
    }

    const char* model_path = argv[1];

    // 初始化whisper
    struct whisper_context_params cparams = whisper_context_default_params();
    struct whisper_context* ctx = whisper_init_from_file_with_params(model_path, cparams);
    if (ctx == nullptr) {
        fprintf(stderr, "Failed to initialize whisper\n");
        return 1;
    }

    // 初始化音频捕获
    void* capture = wasapi_capture_create();
    if (!capture) {
        fprintf(stderr, "Failed to create audio capture\n");
        whisper_free(ctx);
        return 1;
    }

    if (!wasapi_capture_initialize(capture)) {
        fprintf(stderr, "Failed to initialize audio capture\n");
        wasapi_capture_destroy(capture);
        whisper_free(ctx);
        return 1;
    }

    // 设置音频回调
    wasapi_capture_set_callback(capture, (audio_callback)audio_data_callback, &g_audio_buffer);

    // 启动whisper处理线程
    std::thread whisper_thread(whisper_processing_thread, ctx);

    // 启动音频捕获
    if (!wasapi_capture_start(capture)) {
        fprintf(stderr, "Failed to start audio capture\n");
        g_is_running = false;
        whisper_thread.join();
        wasapi_capture_destroy(capture);
        whisper_free(ctx);
        return 1;
    }

    printf("Started capturing. Press Enter to stop...\n");
    getchar();

    // 清理资源
    g_is_running = false;
    whisper_thread.join();
    wasapi_capture_stop(capture);
    wasapi_capture_destroy(capture);
    whisper_free(ctx);

    return 0;
} 
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

// 显示帮助信息
void show_usage(const char* program) {
    fprintf(stderr, "Usage: %s [options] <model_path>\n", program);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -h,  --help            显示帮助信息\n");
    fprintf(stderr, "  -l,  --list            列出可用的音频程序\n");
    fprintf(stderr, "  -p,  --pid <pid>       捕获指定PID的程序音频\n");
    fprintf(stderr, "\nExample:\n");
    fprintf(stderr, "  %s --list                      # 列出可用音频程序\n", program);
    fprintf(stderr, "  %s models/ggml-base.bin        # 捕获系统音频\n", program);
    fprintf(stderr, "  %s -p 1234 models/ggml-base.bin # 捕获PID为1234的程序音频\n", program);
}

// 列出可用的音频程序
void list_audio_applications(void* capture) {
    const int MAX_APPS = 100;
    AudioAppInfo apps[MAX_APPS];
    
    int count = wasapi_capture_get_applications(capture, apps, MAX_APPS);
    
    if (count == 0) {
        printf("没有找到正在播放音频的程序\n");
        return;
    }
    
    printf("\n可用的音频程序列表：\n");
    printf("----------------------------------------\n");
    printf("PID\t程序路径\n");
    printf("----------------------------------------\n");
    
    for (int i = 0; i < count; i++) {
        printf("%u\t%ls\n", apps[i].pid, apps[i].name);
    }
    printf("----------------------------------------\n");
}

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
    bool list_mode = false;
    unsigned int target_pid = 0;
    const char* model_path = nullptr;

    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            show_usage(argv[0]);
            return 0;
        }
        else if (arg == "-l" || arg == "--list") {
            list_mode = true;
        }
        else if (arg == "-p" || arg == "--pid") {
            if (i + 1 < argc) {
                target_pid = std::stoul(argv[++i]);
            } else {
                fprintf(stderr, "Error: --pid 选项需要一个PID参数\n");
                return 1;
            }
        }
        else if (!model_path && arg[0] != '-') {
            model_path = argv[i];
        }
    }

    // 初始化音频捕获
    void* capture = wasapi_capture_create();
    if (!capture) {
        fprintf(stderr, "Failed to create audio capture\n");
        return 1;
    }

    if (!wasapi_capture_initialize(capture)) {
        fprintf(stderr, "Failed to initialize audio capture\n");
        wasapi_capture_destroy(capture);
        return 1;
    }

    // 如果是列表模式，显示程序列表后退出
    if (list_mode) {
        list_audio_applications(capture);
        wasapi_capture_destroy(capture);
        return 0;
    }

    // 检查是否提供了模型路径
    if (!model_path) {
        fprintf(stderr, "Error: 需要提供模型路径\n");
        show_usage(argv[0]);
        wasapi_capture_destroy(capture);
        return 1;
    }

    // 初始化whisper
    struct whisper_context_params cparams = whisper_context_default_params();
    struct whisper_context* ctx = whisper_init_from_file_with_params(model_path, cparams);
    if (ctx == nullptr) {
        fprintf(stderr, "Failed to initialize whisper\n");
        wasapi_capture_destroy(capture);
        return 1;
    }

    // 设置音频回调
    wasapi_capture_set_callback(capture, (audio_callback)audio_data_callback, &g_audio_buffer);

    // 启动whisper处理线程
    std::thread whisper_thread(whisper_processing_thread, ctx);

    // 启动音频捕获
    bool capture_success;
    if (target_pid > 0) {
        printf("正在捕获PID %u 的音频...\n", target_pid);
        capture_success = wasapi_capture_start_process(capture, target_pid);
    } else {
        printf("正在捕获系统音频...\n");
        capture_success = wasapi_capture_start(capture);
    }

    if (!capture_success) {
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
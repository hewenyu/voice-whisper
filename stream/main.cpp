#include "whisper.h"
#include "../audio_capture/windows/wasapi_capture.h"
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <queue>
#include <map>

// 音频缓冲队列
struct AudioBuffer {
    std::mutex mtx;
    std::queue<std::vector<float>> queue;
    static const int MAX_SIZE = 100;  // 最大缓冲数量
};

// whisper参数结构体
struct WhisperParams {
    std::string language = "zh";      // 输入语言
    std::string translate_to = "";    // 翻译目标语言
    bool translate = false;           // 是否翻译
    bool no_timestamps = true;        // 是否显示时间戳
    int threads = 4;                  // 线程数
    int max_tokens = 32;             // 最大token数
    bool use_gpu = true;             // 是否使用GPU
    float vad_thold = 0.6f;          // VAD阈值
    bool print_special = false;       // 是否打印特殊标记
    int step_ms = 3000;              // 音频步长(ms)
    int length_ms = 10000;           // 音频长度(ms)
};

// 语言代码映射
const std::map<std::string, std::string> LANGUAGE_CODES = {
    {"auto", "auto"}, {"en", "english"}, {"zh", "chinese"}, {"ja", "japanese"},
    {"ko", "korean"}, {"fr", "french"}, {"de", "german"}, {"es", "spanish"},
    {"ru", "russian"}, {"it", "italian"}
};

// 全局变量
AudioBuffer g_audio_buffer;
std::atomic<bool> g_is_running{true};
WhisperParams g_params;

// 显示帮助信息
void show_usage(const char* program) {
    fprintf(stderr, "Usage: %s [options] <model_path>\n", program);
    fprintf(stderr, "\n音频捕获选项:\n");
    fprintf(stderr, "  -h,  --help                显示帮助信息\n");
    fprintf(stderr, "  -l,  --list                列出可用的音频程序\n");
    fprintf(stderr, "  -p,  --pid <pid>           捕获指定PID的程序音频\n");
    fprintf(stderr, "\nWhisper选项:\n");
    fprintf(stderr, "  -t,  --threads <n>         使用的线程数 (默认: 4)\n");
    fprintf(stderr, "  -mt, --max-tokens <n>      最大token数 (默认: 32)\n");
    fprintf(stderr, "  -ng, --no-gpu             禁用GPU加速\n");
    fprintf(stderr, "  -l,  --language <lang>     输入音频语言 (默认: zh)\n");
    fprintf(stderr, "  -tr, --translate           启用翻译\n");
    fprintf(stderr, "  -tt, --translate-to <lang> 翻译目标语言 (默认: en)\n");
    fprintf(stderr, "  -ts, --timestamps          显示时间戳\n");
    fprintf(stderr, "  -ps, --print-special       显示特殊标记\n");
    fprintf(stderr, "  -vt, --vad-thold <n>       VAD阈值 [0-1] (默认: 0.6)\n");
    fprintf(stderr, "  -sm, --step-ms <n>         音频步长(ms) (默认: 3000)\n");
    fprintf(stderr, "  -lm, --length-ms <n>       音频长度(ms) (默认: 10000)\n");
    fprintf(stderr, "\n支持的语言:\n");
    for (const auto& lang : LANGUAGE_CODES) {
        fprintf(stderr, "  %-6s : %s\n", lang.first.c_str(), lang.second.c_str());
    }
    fprintf(stderr, "\nExample:\n");
    fprintf(stderr, "  %s --list                                    # 列出可用音频程序\n", program);
    fprintf(stderr, "  %s models/ggml-base.bin                      # 捕获系统音频\n", program);
    fprintf(stderr, "  %s -p 1234 --language en models/ggml-base.bin # 捕获PID为1234的英语音频\n", program);
    fprintf(stderr, "  %s --translate --translate-to ja models/ggml-base.bin # 翻译成日语\n", program);
}

// 验证语言代码
bool is_valid_language(const std::string& lang) {
    return LANGUAGE_CODES.find(lang) != LANGUAGE_CODES.end();
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
    wparams.print_special = g_params.print_special;
    wparams.print_realtime = true;
    wparams.print_timestamps = !g_params.no_timestamps;
    wparams.translate = g_params.translate;
    wparams.language = g_params.language.c_str();
    wparams.n_threads = g_params.threads;
    wparams.max_tokens = g_params.max_tokens;
    wparams.single_segment = true;

    // 如果指定了翻译目标语言
    if (!g_params.translate_to.empty()) {
        wparams.translate = true;
        wparams.language = g_params.translate_to.c_str();
    }

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
        if (audio_data.size() >= WHISPER_SAMPLE_RATE * (g_params.step_ms / 1000)) {
            if (whisper_full(ctx, wparams, audio_data.data(), audio_data.size()) != 0) {
                fprintf(stderr, "Failed to process audio\n");
                continue;
            }

            // 输出识别结果
            const int n_segments = whisper_full_n_segments(ctx);
            for (int i = 0; i < n_segments; ++i) {
                const char* text = whisper_full_get_segment_text(ctx, i);
                if (g_params.no_timestamps) {
                    printf("%s", text);
                } else {
                    const int64_t t0 = whisper_full_get_segment_t0(ctx, i);
                    const int64_t t1 = whisper_full_get_segment_t1(ctx, i);
                    printf("[%d:%02d.%03d -> %d:%02d.%03d] %s\n",
                        (int)(t0 / 60000), (int)((t0 / 1000) % 60), (int)(t0 % 1000),
                        (int)(t1 / 60000), (int)((t1 / 1000) % 60), (int)(t1 % 1000),
                        text);
                }
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
        else if (arg == "-t" || arg == "--threads") {
            if (i + 1 < argc) g_params.threads = std::stoi(argv[++i]);
        }
        else if (arg == "-mt" || arg == "--max-tokens") {
            if (i + 1 < argc) g_params.max_tokens = std::stoi(argv[++i]);
        }
        else if (arg == "-ng" || arg == "--no-gpu") {
            g_params.use_gpu = false;
        }
        else if (arg == "-l" || arg == "--language") {
            if (i + 1 < argc) {
                std::string lang = argv[++i];
                if (!is_valid_language(lang)) {
                    fprintf(stderr, "Error: 不支持的语言代码: %s\n", lang.c_str());
                    return 1;
                }
                g_params.language = lang;
            }
        }
        else if (arg == "-tr" || arg == "--translate") {
            g_params.translate = true;
        }
        else if (arg == "-tt" || arg == "--translate-to") {
            if (i + 1 < argc) {
                std::string lang = argv[++i];
                if (!is_valid_language(lang)) {
                    fprintf(stderr, "Error: 不支持的目标语言代码: %s\n", lang.c_str());
                    return 1;
                }
                g_params.translate_to = lang;
            }
        }
        else if (arg == "-ts" || arg == "--timestamps") {
            g_params.no_timestamps = false;
        }
        else if (arg == "-ps" || arg == "--print-special") {
            g_params.print_special = true;
        }
        else if (arg == "-vt" || arg == "--vad-thold") {
            if (i + 1 < argc) g_params.vad_thold = std::stof(argv[++i]);
        }
        else if (arg == "-sm" || arg == "--step-ms") {
            if (i + 1 < argc) g_params.step_ms = std::stoi(argv[++i]);
        }
        else if (arg == "-lm" || arg == "--length-ms") {
            if (i + 1 < argc) g_params.length_ms = std::stoi(argv[++i]);
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
    cparams.use_gpu = g_params.use_gpu;
    struct whisper_context* ctx = whisper_init_from_file_with_params(model_path, cparams);
    if (ctx == nullptr) {
        fprintf(stderr, "Failed to initialize whisper\n");
        wasapi_capture_destroy(capture);
        return 1;
    }

    // 打印当前设置
    printf("\n当前设置:\n");
    printf("----------------------------------------\n");
    printf("输入语言: %s\n", LANGUAGE_CODES.at(g_params.language).c_str());
    if (g_params.translate) {
        printf("翻译: 开启\n");
        if (!g_params.translate_to.empty()) {
            printf("翻译目标语言: %s\n", LANGUAGE_CODES.at(g_params.translate_to).c_str());
        }
    }
    printf("线程数: %d\n", g_params.threads);
    printf("GPU加速: %s\n", g_params.use_gpu ? "开启" : "关闭");
    printf("时间戳: %s\n", g_params.no_timestamps ? "关闭" : "开启");
    printf("----------------------------------------\n\n");

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
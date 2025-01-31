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
#include <fcntl.h>
#include <io.h>

// 设置控制台UTF-8输出
void set_console_utf8() {
#ifdef _WIN32
    // 设置控制台代码页为UTF-8
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // 启用控制台的虚拟终端序列
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    // 设置标准输出为UTF-8
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
#endif
}

// 音频缓冲队列
struct AudioBuffer {
    std::mutex mtx;
    std::queue<std::vector<float>> queue;
    static const int MAX_SIZE = 100;  // 最大缓冲数量
    int sample_rate = 48000;          // 默认采样率，将在初始化时更新
};

// whisper参数结构体
struct WhisperParams {
    std::string language = "auto";      // 输入语言
    std::string translate_to = "";    // 翻译目标语言
    bool translate = false;           // 是否翻译
    bool no_timestamps = true;        // 是否显示时间戳
    int threads = 8;                  // 线程数
    int max_tokens = 32;             // 最大token数
    bool use_gpu = false;             // 是否使用GPU
    float vad_thold = 0.6f;          // VAD阈值
    bool print_special = false;       // 是否打印特殊标记
    int step_ms = 500;               // 音频步长(ms)
    int length_ms = 5000;            // 音频长度(ms)
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
    // 转换程序名为宽字符
    int len = MultiByteToWideChar(CP_UTF8, 0, program, -1, NULL, 0);
    std::vector<wchar_t> wprogram(len);
    MultiByteToWideChar(CP_UTF8, 0, program, -1, wprogram.data(), len);

    fwprintf(stderr, L"Usage: %ls [options] <model_path>\n", wprogram.data());
    fwprintf(stderr, L"\n音频捕获选项:\n");
    fwprintf(stderr, L"  -h,  --help                显示帮助信息\n");
    fwprintf(stderr, L"  -l,  --list                列出可用的音频程序\n");
    fwprintf(stderr, L"  -p,  --pid <pid>           捕获指定PID的程序音频\n");
    fwprintf(stderr, L"\nWhisper选项:\n");
    fwprintf(stderr, L"  -t,  --threads <n>         使用的线程数 (默认: 8)\n");
    fwprintf(stderr, L"  -mt, --max-tokens <n>      最大token数 (默认: 32)\n");
    fwprintf(stderr, L"  -ng, --no-gpu             禁用GPU加速\n");
    fwprintf(stderr, L"  -l,  --language <lang>     输入音频语言 (默认: zh)\n");
    fwprintf(stderr, L"  -tr, --translate           启用翻译\n");
    fwprintf(stderr, L"  -tt, --translate-to <lang> 翻译目标语言 (默认: en)\n");
    fwprintf(stderr, L"  -ts, --timestamps          显示时间戳\n");
    fwprintf(stderr, L"  -ps, --print-special       显示特殊标记\n");
    fwprintf(stderr, L"  -vt, --vad-thold <n>       VAD阈值 [0-1] (默认: 0.6)\n");
    fwprintf(stderr, L"  -sm, --step-ms <n>         音频步长(ms) (默认: 500)\n");
    fwprintf(stderr, L"  -lm, --length-ms <n>       音频长度(ms) (默认: 5000)\n");
    fwprintf(stderr, L"\n支持的语言:\n");
    
    for (const auto& lang : LANGUAGE_CODES) {
        // 转换语言名称为宽字符
        len = MultiByteToWideChar(CP_UTF8, 0, lang.second.c_str(), -1, NULL, 0);
        std::vector<wchar_t> wlang(len);
        MultiByteToWideChar(CP_UTF8, 0, lang.second.c_str(), -1, wlang.data(), len);
        
        fwprintf(stderr, L"  %-6hs : %ls\n", lang.first.c_str(), wlang.data());
    }
    
    fwprintf(stderr, L"\nExample:\n");
    fwprintf(stderr, L"  %ls --list                                    # 列出可用音频程序\n", wprogram.data());
    fwprintf(stderr, L"  %ls models/ggml-base.bin                      # 捕获系统音频\n", wprogram.data());
    fwprintf(stderr, L"  %ls -p 1234 --language en models/ggml-base.bin # 捕获PID为1234的英语音频\n", wprogram.data());
    fwprintf(stderr, L"  %ls --translate --translate-to ja models/ggml-base.bin # 翻译成日语\n", wprogram.data());
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
        wprintf(L"没有找到正在播放音频的程序\n");
        return;
    }
    
    wprintf(L"\n可用的音频程序列表：\n");
    wprintf(L"----------------------------------------\n");
    wprintf(L"PID\t程序路径\n");
    wprintf(L"----------------------------------------\n");
    
    for (int i = 0; i < count; i++) {
        wprintf(L"%u\t%ls\n", apps[i].pid, apps[i].name);
    }
    wprintf(L"----------------------------------------\n");
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
    wparams.no_context = true;
    wparams.duration_ms = g_params.length_ms;

    // 如果指定了翻译目标语言
    if (!g_params.translate_to.empty()) {
        wparams.translate = true;
        wparams.language = g_params.translate_to.c_str();
    }

    std::vector<float> audio_data;
    std::vector<float> processed_data;  // 用于重采样后的数据
    const int n_samples_step = g_audio_buffer.sample_rate * g_params.step_ms / 1000;
    const int n_samples_len = g_audio_buffer.sample_rate * g_params.length_ms / 1000;
    audio_data.reserve(n_samples_len);

    // 添加调试信息
    wprintf(L"音频配置:\n");
    wprintf(L"采样率: %d Hz\n", g_audio_buffer.sample_rate);
    wprintf(L"步长样本数: %d\n", n_samples_step);
    wprintf(L"长度样本数: %d\n", n_samples_len);

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
        if (audio_data.size() >= n_samples_len) {
            // 检查音频数据是否有效
            bool is_valid = false;
            float max_abs = 0.0f;
            for (float sample : audio_data) {
                max_abs = std::max(max_abs, std::abs(sample));
                if (max_abs > 0.01f) {  // 检查是否有有效的音频信号
                    is_valid = true;
                    break;
                }
            }

            if (!is_valid) {
                wprintf(L"跳过静音音频段\n");
                audio_data.clear();
                audio_data.reserve(n_samples_len);
                continue;
            }

            // 重采样到16kHz
            processed_data.resize(audio_data.size() * WHISPER_SAMPLE_RATE / g_audio_buffer.sample_rate);
            for (size_t i = 0; i < processed_data.size(); i++) {
                float src_idx = i * g_audio_buffer.sample_rate / (float)WHISPER_SAMPLE_RATE;
                size_t src_idx_floor = (size_t)src_idx;
                float t = src_idx - src_idx_floor;
                
                if (src_idx_floor >= audio_data.size() - 1) {
                    processed_data[i] = audio_data.back();
                } else {
                    processed_data[i] = audio_data[src_idx_floor] * (1 - t) + 
                                      audio_data[src_idx_floor + 1] * t;
                }
            }

            // 处理音频
            if (whisper_full(ctx, wparams, processed_data.data(), processed_data.size()) != 0) {
                fwprintf(stderr, L"Failed to process audio (samples: %zu, max amplitude: %.3f)\n", 
                    processed_data.size(), max_abs);
                continue;
            }

            // 输出识别结果
            const int n_segments = whisper_full_n_segments(ctx);
            if (n_segments > 0) {
                for (int i = 0; i < n_segments; ++i) {
                    const char* text = whisper_full_get_segment_text(ctx, i);
                    if (text == nullptr || strlen(text) == 0) {
                        continue;
                    }
                    
                    // 将UTF-8文本转换为宽字符
                    int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
                    std::vector<wchar_t> wtext(len);
                    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext.data(), len);

                    if (g_params.no_timestamps) {
                        wprintf(L"%ls", wtext.data());
                    } else {
                        const int64_t t0 = whisper_full_get_segment_t0(ctx, i);
                        const int64_t t1 = whisper_full_get_segment_t1(ctx, i);
                        wprintf(L"[%d:%02d.%03d -> %d:%02d.%03d] %ls\n",
                            (int)(t0 / 60000), (int)((t0 / 1000) % 60), (int)(t0 % 1000),
                            (int)(t1 / 60000), (int)((t1 / 1000) % 60), (int)(t1 % 1000),
                            wtext.data());
                    }
                    fflush(stdout);
                }
                wprintf(L"\n");
            }

            // 保留最后一部分音频用于下一次处理
            const int n_samples_keep = n_samples_len - n_samples_step;
            if (n_samples_keep > 0 && audio_data.size() > n_samples_keep) {
                std::vector<float> new_audio(audio_data.end() - n_samples_keep, audio_data.end());
                audio_data = std::move(new_audio);
            } else {
                audio_data.clear();
            }
            audio_data.reserve(n_samples_len);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int main(int argc, char** argv) {
    // 设置控制台UTF-8支持
    set_console_utf8();

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

    // 获取音频格式
    AudioFormat format;
    if (!wasapi_capture_get_format(capture, &format)) {
        fprintf(stderr, "Failed to get audio format\n");
        wasapi_capture_destroy(capture);
        return 1;
    }
    g_audio_buffer.sample_rate = format.sample_rate;

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
    wprintf(L"\n当前设置:\n");
    wprintf(L"----------------------------------------\n");
    wprintf(L"输入语言: %hs\n", LANGUAGE_CODES.at(g_params.language).c_str());
    if (g_params.translate) {
        wprintf(L"翻译: 开启\n");
        if (!g_params.translate_to.empty()) {
            wprintf(L"翻译目标语言: %hs\n", LANGUAGE_CODES.at(g_params.translate_to).c_str());
        }
    }
    wprintf(L"线程数: %d\n", g_params.threads);
    wprintf(L"GPU加速: %ls\n", g_params.use_gpu ? L"开启" : L"关闭");
    wprintf(L"时间戳: %ls\n", g_params.no_timestamps ? L"关闭" : L"开启");
    wprintf(L"音频步长: %d ms\n", g_params.step_ms);
    wprintf(L"音频长度: %d ms\n", g_params.length_ms);
    wprintf(L"----------------------------------------\n\n");

    // 设置音频回调
    wasapi_capture_set_callback(capture, (audio_callback)audio_data_callback, &g_audio_buffer);

    // 启动whisper处理线程
    std::thread whisper_thread(whisper_processing_thread, ctx);

    // 启动音频捕获
    bool capture_success;
    if (target_pid > 0) {
        wprintf(L"正在捕获PID %u 的音频...\n", target_pid);
        capture_success = wasapi_capture_start_process(capture, target_pid);
    } else {
        wprintf(L"正在捕获系统音频...\n");
        capture_success = wasapi_capture_start(capture);
    }

    if (!capture_success) {
        fwprintf(stderr, L"Failed to start audio capture\n");
        g_is_running = false;
        whisper_thread.join();
        wasapi_capture_destroy(capture);
        whisper_free(ctx);
        return 1;
    }

    wprintf(L"Started capturing. Press Enter to stop...\n");
    getchar();

    // 清理资源
    g_is_running = false;
    whisper_thread.join();
    wasapi_capture_stop(capture);
    wasapi_capture_destroy(capture);
    whisper_free(ctx);

    return 0;
} 
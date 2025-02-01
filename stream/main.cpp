#include "whisper.h"
#include <SDL2/SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <queue>
#include <map>
#include <cmath>

// 音频缓冲队列
struct AudioBuffer {
    std::mutex mtx;
    std::queue<std::vector<float>> queue;
    static const int MAX_SIZE = 100;  // 最大缓冲数量
};

// whisper参数结构体
struct WhisperParams {
    int32_t n_threads = std::min(4, (int32_t)std::thread::hardware_concurrency());
    int32_t step_ms = 3000;      // 音频步长(ms)
    int32_t length_ms = 10000;   // 音频长度(ms)
    int32_t keep_ms = 200;       // 保留音频长度(ms)
    int32_t capture_id = -1;     // 音频设备ID
    int32_t max_tokens = 32;     // 最大token数
    int32_t audio_ctx = 0;       // 音频上下文大小

    float vad_thold = 0.6f;      // VAD阈值
    float freq_thold = 100.0f;   // 频率阈值

    bool translate = false;       // 是否翻译
    bool no_fallback = false;    // 是否禁用温度回退
    bool print_special = false;  // 是否打印特殊标记
    bool no_context = true;      // 是否保持上下文
    bool no_timestamps = false;  // 是否显示时间戳
    bool use_gpu = true;        // 是否使用GPU

    const char* language = "auto"; // 默认使用auto自动检测
    const char* model = "models/ggml-base.en.bin"; // 默认模型路径
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

// SDL音频回调函数
void audio_callback(void* userdata, Uint8* stream, int len) {
    auto& audio_buffer = *static_cast<AudioBuffer*>(userdata);
    
    // 将音频数据转换为float
    int num_samples = len / sizeof(float);
    std::vector<float> samples(num_samples);
    float* float_stream = reinterpret_cast<float*>(stream);
    
    for (int i = 0; i < num_samples; i++) {
        samples[i] = float_stream[i];
    }
    
    std::lock_guard<std::mutex> lock(audio_buffer.mtx);
    if (audio_buffer.queue.size() < AudioBuffer::MAX_SIZE) {
        audio_buffer.queue.push(std::move(samples));
    }
}

// 列出音频设备
void list_audio_devices() {
    int count = SDL_GetNumAudioDevices(1);  // 1 表示录制设备
    printf("\n可用的音频设备:\n");
    printf("----------------------------------------\n");
    for (int i = 0; i < count; i++) {
        const char* name = SDL_GetAudioDeviceName(i, 1);
        printf("%d: %s\n", i, name);
    }
    printf("----------------------------------------\n");
}

// 查找虚拟音频电缆设备
int find_virtual_audio_cable() {
    int count = SDL_GetNumAudioDevices(1);
    for (int i = 0; i < count; i++) {
        const char* name = SDL_GetAudioDeviceName(i, 1);
        // 检查设备名称是否包含常见的虚拟音频电缆关键字
        if (strstr(name, "CABLE Output") != nullptr ||
            strstr(name, "VB-Audio") != nullptr ||
            strstr(name, "Virtual Cable") != nullptr) {
            return i;
        }
    }
    return -1;
}

// 显示帮助信息
void show_usage(const char* program) {
    printf("Usage: %s [options] <model_path>\n\n", program);
    printf("选项:\n");
    printf("  -h,  --help                显示帮助信息\n");
    printf("  -l,  --list                列出可用的音频设备\n");
    printf("  -d,  --device N            选择音频设备ID (默认: 自动检测虚拟音频电缆)\n");
    printf("  -t,  --threads N           使用的线程数 (默认: 4)\n");
    printf("  -mt, --max-tokens N        最大token数 (默认: 32)\n");
    printf("  -ng, --no-gpu              禁用GPU加速\n");
    printf("  -l,  --language LANG       输入音频语言 (默认: auto)\n");
    printf("  -tr, --translate           启用翻译\n");
    printf("  -ts, --timestamps          显示时间戳\n");
    printf("  -vt, --vad-thold N         VAD阈值 [0-1] (默认: 0.6)\n");
    printf("  -sm, --step-ms N           音频步长(ms) (默认: 3000)\n");
    printf("  -lm, --length-ms N         音频长度(ms) (默认: 10000)\n");
}

int main(int argc, char** argv) {
    // 设置控制台UTF-8编码
    #ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    #endif

    // 解析命令行参数
    bool list_mode = false;
    int device_id = 0;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            show_usage(argv[0]);
            return 0;
        }
        else if (arg == "-l" || arg == "--list") {
            list_mode = true;
        }
        else if (arg == "-d" || arg == "--device") {
            if (i + 1 < argc) {
                device_id = std::stoi(argv[++i]);
            }
        }
        else if (arg == "-t" || arg == "--threads") {
            if (i + 1 < argc) g_params.n_threads = std::stoi(argv[++i]);
        }
        else if (arg == "-mt" || arg == "--max-tokens") {
            if (i + 1 < argc) g_params.max_tokens = std::stoi(argv[++i]);
        }
        else if (arg == "-ng" || arg == "--no-gpu") {
            g_params.use_gpu = false;
        }
        else if (arg == "-l" || arg == "--language") {
            if (i + 1 < argc) g_params.language = argv[++i];
        }
        else if (arg == "-tr" || arg == "--translate") {
            g_params.translate = true;
        }
        else if (arg == "-vt" || arg == "--vad-thold") {
            if (i + 1 < argc) g_params.vad_thold = std::stof(argv[++i]);
        }
        else if (!g_params.model) {
            g_params.model = argv[i];
        }
    }

    // 如果没有指定设备ID，尝试自动检测虚拟音频电缆
    if (device_id == 0) {
        int vac_id = find_virtual_audio_cable();
        if (vac_id >= 0) {
            device_id = vac_id;
            printf("已自动检测到虚拟音频电缆设备 (ID: %d)\n", device_id);
        } else {
            printf("警告: 未检测到虚拟音频电缆设备，将使用默认设备\n");
            printf("请确保已安装虚拟音频电缆软件(如 VB-CABLE)，并将要录制的应用程序输出设置为虚拟音频电缆\n");
            list_audio_devices();
        }
    }

    // 初始化SDL
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL初始化失败: %s\n", SDL_GetError());
        return 1;
    }

    // 如果是列表模式，显示设备列表后退出
    if (list_mode) {
        list_audio_devices();
        SDL_Quit();
        return 0;
    }

    // 检查是否提供了模型路径
    if (!g_params.model) {
        fprintf(stderr, "错误: 需要提供模型路径\n");
        show_usage(argv[0]);
        SDL_Quit();
        return 1;
    }

    // 设置SDL音频参数
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = WHISPER_SAMPLE_RATE;
    want.format = AUDIO_F32;
    want.channels = 1;
    want.samples = 1024;
    want.callback = audio_callback;
    want.userdata = &g_audio_buffer;

    // 获取设备名称
    const char* device_name = SDL_GetAudioDeviceName(device_id, 1);
    if (!device_name) {
        fprintf(stderr, "错误: 无效的音频设备ID: %d\n", device_id);
        list_audio_devices();
        SDL_Quit();
        return 1;
    }

    // 打开音频设备
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(device_name, 1, &want, &have, 0);
    if (dev == 0) {
        fprintf(stderr, "无法打开音频设备 '%s': %s\n", device_name, SDL_GetError());
        SDL_Quit();
        return 1;
    }

    printf("使用音频设备: %s\n", device_name);

    // 初始化whisper
    struct whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = g_params.use_gpu;
    
    struct whisper_context* ctx = whisper_init_from_file_with_params(g_params.model, cparams);
    if (ctx == nullptr) {
        fprintf(stderr, "无法加载模型: %s\n", g_params.model);
        SDL_CloseAudioDevice(dev);
        SDL_Quit();
        return 1;
    }

    // 开始录音
    SDL_PauseAudioDevice(dev, 0);
    printf("[开始录音]\n");

    // 音频处理参数
    const int n_samples_step = (1e-3 * g_params.step_ms) * WHISPER_SAMPLE_RATE;
    const int n_samples_len = (1e-3 * g_params.length_ms) * WHISPER_SAMPLE_RATE;
    const int n_samples_keep = (1e-3 * g_params.keep_ms) * WHISPER_SAMPLE_RATE;

    std::vector<float> pcmf32;
    std::vector<float> pcmf32_old;
    std::vector<float> pcmf32_new;

    pcmf32.reserve(n_samples_len);
    pcmf32_new.reserve(n_samples_len);

    printf("\n处理参数:\n");
    printf("采样率: %d Hz\n", WHISPER_SAMPLE_RATE);
    printf("步长: %.1f ms\n", float(n_samples_step) / WHISPER_SAMPLE_RATE * 1000);
    printf("长度: %.1f ms\n", float(n_samples_len) / WHISPER_SAMPLE_RATE * 1000);
    printf("线程数: %d\n", g_params.n_threads);
    printf("语言: %s\n", g_params.language);
    printf("任务: %s\n\n", g_params.translate ? "翻译" : "转写");

    while (g_is_running) {
        // 从音频缓冲区获取数据
        {
            std::lock_guard<std::mutex> lock(g_audio_buffer.mtx);
            while (!g_audio_buffer.queue.empty()) {
                const auto& chunk = g_audio_buffer.queue.front();
                pcmf32_new.insert(pcmf32_new.end(), chunk.begin(), chunk.end());
                g_audio_buffer.queue.pop();
            }
        }

        if (pcmf32_new.size() > 2 * n_samples_step) {
            fprintf(stderr, "\n警告: 音频处理速度不够快，丢弃部分音频...\n");
            pcmf32_new.clear();
            continue;
        }

        if (pcmf32_new.size() >= n_samples_step) {
            // 处理新的音频数据
            const int n_samples_new = pcmf32_new.size();
            
            // 从上一次迭代中保留部分音频
            const int n_samples_take = std::min((int)pcmf32_old.size(), 
                                              std::max(0, n_samples_keep + n_samples_len - n_samples_new));

            pcmf32.resize(n_samples_new + n_samples_take);

            // 复制保留的音频数据
            for (int i = 0; i < n_samples_take; i++) {
                pcmf32[i] = pcmf32_old[pcmf32_old.size() - n_samples_take + i];
            }

            // 复制新的音频数据
            std::copy(pcmf32_new.begin(), pcmf32_new.end(), pcmf32.begin() + n_samples_take);

            // 保存当前音频数据供下次使用
            pcmf32_old = pcmf32;
            pcmf32_new.clear();

            // VAD检测
            bool speech_detected = false;
            const int n_samples_per_ms = WHISPER_SAMPLE_RATE / 1000;
            const int n_samples_window = 100 * n_samples_per_ms; // 100ms窗口

            for (int i = 0; i + n_samples_window < (int)pcmf32.size(); i += n_samples_window) {
                float sum = 0.0f;
                for (int j = 0; j < n_samples_window; j++) {
                    sum += std::abs(pcmf32[i + j]);
                }
                float avg = sum / n_samples_window;
                if (avg > g_params.vad_thold) {
                    speech_detected = true;
                    break;
                }
            }

            if (!speech_detected) {
                continue;
            }

            // 处理音频
            whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
            wparams.print_progress = false;
            wparams.print_special = g_params.print_special;
            wparams.print_realtime = true;
            wparams.print_timestamps = !g_params.no_timestamps;
            wparams.translate = g_params.translate;
            wparams.language = g_params.language;
            wparams.n_threads = g_params.n_threads;
            wparams.audio_ctx = g_params.audio_ctx;
            wparams.max_tokens = g_params.max_tokens;

            if (whisper_full(ctx, wparams, pcmf32.data(), pcmf32.size()) != 0) {
                fprintf(stderr, "处理音频失败\n");
                continue;
            }

            // 输出识别结果
            const int n_segments = whisper_full_n_segments(ctx);
            for (int i = 0; i < n_segments; ++i) {
                const char* text = whisper_full_get_segment_text(ctx, i);
                const int64_t t0 = whisper_full_get_segment_t0(ctx, i);
                const int64_t t1 = whisper_full_get_segment_t1(ctx, i);

                if (strlen(text) > 0) {
                    if (g_params.no_timestamps) {
                        printf("%s", text);
                    } else {
                        printf("[%d:%02d.%03d -> %d:%02d.%03d] %s\n",
                            (int)(t0 / 60000), (int)((t0 / 1000) % 60), (int)(t0 % 1000),
                            (int)(t1 / 60000), (int)((t1 / 1000) % 60), (int)(t1 % 1000),
                            text);
                    }
                    fflush(stdout);
                }
            }
        }

        SDL_Delay(10); // 避免CPU占用过高
    }

    // 清理资源
    whisper_free(ctx);
    SDL_CloseAudioDevice(dev);
    SDL_Quit();

    return 0;
} 
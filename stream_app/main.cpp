#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// whisper includes
#include "whisper.h"

// standard includes
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <fstream>
#include <iostream>
#include <cmath>
#include <mutex>
#include <cstring>
#include <deque>

// wasapi capture
#include "../audio_capture/windows/wasapi_capture.h"

// constants
#define COMMON_SAMPLE_RATE 16000

// wav writer for saving audio
class wav_writer {
public:
    wav_writer() : file(nullptr) {}
    ~wav_writer() { close(); }

    bool open(const std::string & fname, int sample_rate, int channels, int bits_per_sample) {
        close();
        file = fopen(fname.c_str(), "wb");
        if (!file) return false;

        // write WAV header
        uint32_t data_size = 0;
        uint32_t file_size = data_size + 44 - 8;

        // RIFF header
        fwrite("RIFF", 1, 4, file);
        fwrite(&file_size, 1, 4, file);
        fwrite("WAVE", 1, 4, file);

        // fmt chunk
        uint16_t fmt_chunk[] = {
            0x666D, 0x7420, 0x1000, 0x0000, 0x0100, 0x0100, 0x44AC, 0x0000,
            0x88, 0x58, 0x01, 0x00, 0x0200, 0x1000
        };
        fmt_chunk[4] = channels;
        fmt_chunk[5] = bits_per_sample;
        fmt_chunk[6] = sample_rate;
        fmt_chunk[7] = sample_rate * channels * bits_per_sample / 8;
        fmt_chunk[8] = channels * bits_per_sample / 8;
        fwrite(fmt_chunk, 1, sizeof(fmt_chunk), file);

        // data chunk
        fwrite("data", 1, 4, file);
        fwrite(&data_size, 1, 4, file);

        return true;
    }

    void write(const float * data, size_t n) {
        if (!file) return;

        // convert float to int16_t
        std::vector<int16_t> buf(n);
        for (size_t i = 0; i < n; i++) {
            buf[i] = data[i] * 32768.0f;
        }

        fwrite(buf.data(), 2, n, file);
    }

    void close() {
        if (file) {
            // update WAV header with final size
            long file_size = ftell(file);
            long data_size = file_size - 44;

            fseek(file, 4, SEEK_SET);
            uint32_t size = file_size - 8;
            fwrite(&size, 1, 4, file);

            fseek(file, 40, SEEK_SET);
            fwrite(&data_size, 1, 4, file);

            fclose(file);
            file = nullptr;
        }
    }

private:
    FILE * file;
};

// helper function to convert time to string
std::string to_timestamp(int64_t t, bool comma = false) {
    int64_t msec = t * 10;
    int64_t hr = msec / (1000 * 60 * 60);
    msec = msec - hr * (1000 * 60 * 60);
    int64_t min = msec / (1000 * 60);
    msec = msec - min * (1000 * 60);
    int64_t sec = msec / 1000;
    msec = msec - sec * 1000;

    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d%s%03d", (int) hr, (int) min, (int) sec, comma ? "," : ".", (int) msec);
    return std::string(buf);
}

// VAD function
bool vad_simple(std::vector<float> & pcmf32, int sample_rate, int last_ms, float vad_thold, float freq_thold, bool verbose) {
    const int n_samples      = pcmf32.size();
    const int n_samples_last = (sample_rate * last_ms) / 1000;

    if (n_samples_last >= n_samples) {
        return false;
    }

    float energy_all  = 0.0f;
    float energy_last = 0.0f;

    for (int i = 0; i < n_samples; i++) {
        energy_all += fabsf(pcmf32[i]);

        if (i >= n_samples - n_samples_last) {
            energy_last += fabsf(pcmf32[i]);
        }
    }

    energy_all  /= n_samples;
    energy_last /= n_samples_last;

    if (verbose) {
        fprintf(stderr, "%s: energy_all: %f, energy_last: %f, vad_thold: %f, freq_thold: %f\n", __func__, energy_all, energy_last, vad_thold, freq_thold);
    }

    if (energy_last > vad_thold*energy_all) {
        return false;
    }

    return true;
}

// command-line parameters
struct whisper_params {
    int32_t n_threads  = std::min(4, (int32_t) std::thread::hardware_concurrency());
    int32_t step_ms    = 3000;
    int32_t length_ms  = 10000;
    int32_t keep_ms    = 200;
    int32_t capture_id = -1;
    int32_t max_tokens = 32;
    int32_t audio_ctx  = 0;
    int32_t app_pid    = 0;

    float vad_thold    = 0.6f;
    float freq_thold   = 100.0f;

    bool translate     = false;
    bool no_fallback   = false;
    bool print_special = false;
    bool no_context    = true;
    bool no_timestamps = false;
    bool tinydiarize   = false;
    bool save_audio    = false;
    bool use_gpu       = true;
    bool flash_attn    = false;
    bool list_apps     = false;

    std::string language  = "en";
    std::string model    = "../models/ggml-base.en.bin";
    std::string fname_out;
};

void whisper_print_usage(int argc, char ** argv, const whisper_params & params);

bool whisper_params_parse(int argc, char ** argv, whisper_params & params) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            whisper_print_usage(argc, argv, params);
            exit(0);
        }
        else if (arg == "-t"   || arg == "--threads")     { params.n_threads     = std::stoi(argv[++i]); }
        else if (                  arg == "--step")        { params.step_ms       = std::stoi(argv[++i]); }
        else if (                  arg == "--length")      { params.length_ms     = std::stoi(argv[++i]); }
        else if (arg == "-c"   || arg == "--capture")     { params.capture_id    = std::stoi(argv[++i]); }
        else if (arg == "-mt"  || arg == "--max-tokens")  { params.max_tokens    = std::stoi(argv[++i]); }
        else if (arg == "-ac"  || arg == "--audio-ctx")   { params.audio_ctx     = std::stoi(argv[++i]); }
        else if (arg == "-vth" || arg == "--vad-thold")   { params.vad_thold     = std::stof(argv[++i]); }
        else if (arg == "-fth" || arg == "--freq-thold")  { params.freq_thold    = std::stof(argv[++i]); }
        else if (arg == "-tr"  || arg == "--translate")   { params.translate     = true; }
        else if (arg == "-ps"  || arg == "--print-special") { params.print_special = true; }
        else if (arg == "-kc"  || arg == "--keep-context") { params.no_context    = false; }
        else if (arg == "-l"   || arg == "--language")    { params.language      = argv[++i]; }
        else if (arg == "-m"   || arg == "--model")       { params.model         = argv[++i]; }
        else if (arg == "-f"   || arg == "--file")        { params.fname_out     = argv[++i]; }
        else if (arg == "-nt"  || arg == "--no-timestamps") { params.no_timestamps = true; }
        else if (arg == "-tdrz" || arg == "--tinydiarize")   { params.tinydiarize   = true; }
        else if (arg == "-sa"   || arg == "--save-audio")    { params.save_audio    = true; }
        else if (arg == "-ng"   || arg == "--no-gpu")        { params.use_gpu       = false; }
        else if (arg == "-fa"   || arg == "--flash-attn")    { params.flash_attn    = true; }
        else if (arg == "-la"   || arg == "--list-apps")     { params.list_apps     = true; }
        else if (arg == "-pid"  || arg == "--app-pid")       { params.app_pid       = std::stoi(argv[++i]); }
        else {
            fprintf(stderr, "error: unknown argument: %s\n", arg.c_str());
            whisper_print_usage(argc, argv, params);
            exit(0);
        }
    }

    return true;
}

void whisper_print_usage(int /*argc*/, char ** argv, const whisper_params & params) {
    fprintf(stderr, "\n");
    fprintf(stderr, "usage: %s [options]\n", argv[0]);
    fprintf(stderr, "\n");
    fprintf(stderr, "options:\n");
    fprintf(stderr, "  -h,       --help          show this help message and exit\n");
    fprintf(stderr, "  -t N,     --threads N     number of threads to use during computation (default: %d)\n", params.n_threads);
    fprintf(stderr, "            --step N        audio step size in milliseconds (default: %d)\n", params.step_ms);
    fprintf(stderr, "            --length N      audio length in milliseconds (default: %d)\n", params.length_ms);
    fprintf(stderr, "  -c ID,    --capture ID    capture device ID (default: %d)\n", params.capture_id);
    fprintf(stderr, "  -mt N,    --max-tokens N  maximum number of tokens per audio chunk (default: %d)\n", params.max_tokens);
    fprintf(stderr, "  -ac N,    --audio-ctx N   audio context size (0 - all) (default: %d)\n", params.audio_ctx);
    fprintf(stderr, "  -vth N,   --vad-thold N   voice activity detection threshold (default: %f)\n", params.vad_thold);
    fprintf(stderr, "  -fth N,   --freq-thold N  high-pass frequency cutoff (default: %f)\n", params.freq_thold);
    fprintf(stderr, "  -tr,      --translate     translate from source language to english\n");
    fprintf(stderr, "  -ps,      --print-special print special tokens\n");
    fprintf(stderr, "  -kc,      --keep-context  keep context between audio chunks\n");
    fprintf(stderr, "  -nt,      --no-timestamps do not print timestamps\n");
    fprintf(stderr, "  -l LANG,  --language LANG spoken language (default: %s)\n", params.language.c_str());
    fprintf(stderr, "  -m FNAME, --model FNAME   model path (default: %s)\n", params.model.c_str());
    fprintf(stderr, "  -f FNAME, --file FNAME    output file path (default: %s)\n", params.fname_out.c_str());
    fprintf(stderr, "  -tdrz,    --tinydiarize   [%-7s] enable tinydiarize (requires a tdrz model)\n",     params.tinydiarize ? "true" : "false");
    fprintf(stderr, "  -sa,      --save-audio    [%-7s] save the recorded audio to a file\n",              params.save_audio ? "true" : "false");
    fprintf(stderr, "  -ng,      --no-gpu        [%-7s] disable GPU inference\n",                          params.use_gpu ? "false" : "true");
    fprintf(stderr, "  -fa,      --flash-attn    [%-7s] flash attention during inference\n",               params.flash_attn ? "true" : "false");
    fprintf(stderr, "  -la,      --list-apps     list available applications for capture\n");
    fprintf(stderr, "  -pid N,   --app-pid N     capture audio from specific application PID\n");
    fprintf(stderr, "\n");
}

// helper function to check if we should continue running
bool should_continue() {
    static auto last_check = std::chrono::high_resolution_clock::now();
    auto now = std::chrono::high_resolution_clock::now();
    
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_check).count() > 100) {
        last_check = now;
        // check for Ctrl+C
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            return false;
        }
    }
    return true;
}

int main(int argc, char ** argv) {
    whisper_params params;

    if (whisper_params_parse(argc, argv, params) == false) {
        return 1;
    }

    // 如果只是列出应用程序，则执行后退出
    if (params.list_apps) {
        void* handle = wasapi_capture_create();
        if (!handle) {
            fprintf(stderr, "error: failed to create audio capture\n");
            return 1;
        }

        if (!wasapi_capture_initialize(handle)) {
            fprintf(stderr, "error: failed to initialize audio capture\n");
            wasapi_capture_destroy(handle);
            return 1;
        }

        const int MAX_APPS = 100;
        AudioAppInfo apps[MAX_APPS];
        int count = wasapi_capture_get_applications(handle, apps, MAX_APPS);

        printf("\nAvailable applications for audio capture:\n");
        printf("----------------------------------------\n");
        for (int i = 0; i < count; i++) {
            printf("PID: %u - %ls\n", apps[i].pid, apps[i].name);
        }
        printf("----------------------------------------\n");
        printf("Use --app-pid <PID> to capture audio from a specific application\n\n");

        wasapi_capture_destroy(handle);
        return 0;
    }

    params.keep_ms   = std::min(params.keep_ms,   params.step_ms);
    params.length_ms = std::max(params.length_ms, params.step_ms);

    const int n_samples_step = (1e-3*params.step_ms  )*WHISPER_SAMPLE_RATE;
    const int n_samples_len  = (1e-3*params.length_ms)*WHISPER_SAMPLE_RATE;
    const int n_samples_keep = (1e-3*params.keep_ms  )*WHISPER_SAMPLE_RATE;
    const int n_samples_30s  = (1e-3*30000.0         )*WHISPER_SAMPLE_RATE;

    const bool use_vad = n_samples_step <= 0; // sliding window mode uses VAD

    const int n_new_line = !use_vad ? std::max(1, params.length_ms / params.step_ms - 1) : 1; // number of steps to print new line

    params.no_timestamps  = !use_vad;
    params.no_context    |= use_vad;
    params.max_tokens     = 0;

    // init audio
    audio_async_wasapi audio(params.length_ms);
    if (!audio.init(params.capture_id, COMMON_SAMPLE_RATE)) {
        fprintf(stderr, "error: failed to initialize audio capture\n");
        return 3;
    }

    // 如果指定了应用程序 PID，则启动特定应用程序的捕获
    if (params.app_pid > 0) {
        void* handle = wasapi_capture_create();
        if (!handle) {
            fprintf(stderr, "error: failed to create audio capture\n");
            return 1;
        }

        if (!wasapi_capture_initialize(handle)) {
            fprintf(stderr, "error: failed to initialize audio capture\n");
            wasapi_capture_destroy(handle);
            return 1;
        }

        if (!wasapi_capture_start_process(handle, params.app_pid)) {
            fprintf(stderr, "error: failed to start capturing from PID %d\n", params.app_pid);
            wasapi_capture_destroy(handle);
            return 1;
        }

        printf("Successfully started capturing audio from PID %d\n", params.app_pid);
        wasapi_capture_destroy(handle);
    }

    audio.resume();

    // whisper init
    if (params.language != "auto" && whisper_lang_id(params.language.c_str()) == -1) {
        fprintf(stderr, "error: unknown language '%s'\n", params.language.c_str());
        whisper_print_usage(argc, argv, params);
        exit(0);
    }

    struct whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = params.use_gpu;
    struct whisper_context * ctx = whisper_init_from_file_with_params(params.model.c_str(), cparams);

    std::vector<float> pcmf32    (n_samples_30s, 0.0f);
    std::vector<float> pcmf32_old;
    std::vector<float> pcmf32_new(n_samples_30s, 0.0f);
    std::vector<whisper_token> prompt_tokens;

    // print some info about the processing
    {
        fprintf(stderr, "\n");
        if (!whisper_is_multilingual(ctx)) {
            if (params.language != "en" || params.translate) {
                params.language = "en";
                params.translate = false;
                fprintf(stderr, "%s: WARNING: model is not multilingual, ignoring language and translation options\n", __func__);
            }
        }
        fprintf(stderr, "%s: processing %d samples (step = %.1f sec / len = %.1f sec / keep = %.1f sec), %d threads, lang = %s, task = %s, timestamps = %d ...\n",
                __func__,
                n_samples_step,
                float(n_samples_step)/WHISPER_SAMPLE_RATE,
                float(n_samples_len )/WHISPER_SAMPLE_RATE,
                float(n_samples_keep)/WHISPER_SAMPLE_RATE,
                params.n_threads,
                params.language.c_str(),
                params.translate ? "translate" : "transcribe",
                params.no_timestamps ? 0 : 1);

        if (!use_vad) {
            fprintf(stderr, "%s: n_new_line = %d, no_context = %d\n", __func__, n_new_line, params.no_context);
        } else {
            fprintf(stderr, "%s: using VAD, will transcribe on speech activity\n", __func__);
        }

        fprintf(stderr, "\n");
    }

    int n_iter = 0;
    bool is_running = true;
    bool is_recording = false;

    std::ofstream fout;
    if (params.fname_out.length() > 0) {
        fout.open(params.fname_out);
        if (!fout.is_open()) {
            fprintf(stderr, "%s: failed to open output file '%s'!\n", __func__, params.fname_out.c_str());
            return 1;
        }
    }

    wav_writer wavWriter;
    // save wav file
    if (params.save_audio) {
        // Get current date/time for filename
        time_t now = time(0);
        char buffer[80];
        strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", localtime(&now));
        std::string filename = std::string(buffer) + ".wav";

        wavWriter.open(filename, WHISPER_SAMPLE_RATE, 1, 16);
    }

    printf("[Start speaking]\n");
    fflush(stdout);

    auto t_last  = std::chrono::high_resolution_clock::now();
    const auto t_start = t_last;

    // main audio loop
    while (is_running) {
        // handle Ctrl+C or ESC
        is_running = should_continue();

        if (!is_running) {
            break;
        }

        // process new audio
        if (!use_vad) {
            while (true) {
                audio.get(params.step_ms, pcmf32_new);

                if ((int) pcmf32_new.size() > 2*n_samples_step) {
                    fprintf(stderr, "\n\n%s: WARNING: cannot process audio fast enough, dropping audio ...\n\n", __func__);
                    audio.clear();
                    continue;
                }

                if ((int) pcmf32_new.size() >= n_samples_step) {
                    audio.clear();
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            const int n_samples_new = pcmf32_new.size();

            // take up to params.length_ms audio from previous iteration
            const int n_samples_take = std::min((int) pcmf32_old.size(), std::max(0, n_samples_keep + n_samples_len - n_samples_new));

            //printf("processing: take = %d, new = %d, old = %d\n", n_samples_take, n_samples_new, (int) pcmf32_old.size());

            pcmf32.resize(n_samples_new + n_samples_take);

            for (int i = 0; i < n_samples_take; i++) {
                pcmf32[i] = pcmf32_old[pcmf32_old.size() - n_samples_take + i];
            }

            memcpy(pcmf32.data() + n_samples_take, pcmf32_new.data(), n_samples_new*sizeof(float));

            pcmf32_old = pcmf32;
        } else {
            const auto t_now  = std::chrono::high_resolution_clock::now();
            const auto t_diff = std::chrono::duration_cast<std::chrono::milliseconds>(t_now - t_last).count();

            if (t_diff < 2000) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                continue;
            }

            audio.get(2000, pcmf32_new);

            if (vad_simple(pcmf32_new, WHISPER_SAMPLE_RATE, 1000, params.vad_thold, params.freq_thold, false)) {
                audio.get(params.length_ms, pcmf32);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                continue;
            }

            t_last = t_now;
        }

        // save wav file
        if (params.save_audio) {
            wavWriter.write(pcmf32_new.data(), pcmf32_new.size());
        }

        // run the inference
        {
            whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

            wparams.print_progress   = false;
            wparams.print_special    = params.print_special;
            wparams.print_realtime   = false;
            wparams.print_timestamps = !params.no_timestamps;
            wparams.translate        = params.translate;
            wparams.single_segment   = !use_vad;
            wparams.max_tokens       = params.max_tokens;
            wparams.language         = params.language.c_str();
            wparams.n_threads        = params.n_threads;

            wparams.audio_ctx        = params.audio_ctx;
            wparams.tdrz_enable      = params.tinydiarize;

            // disable temperature fallback
            wparams.temperature_inc  = params.no_fallback ? 0.0f : wparams.temperature_inc;

            wparams.prompt_tokens    = params.no_context ? nullptr : prompt_tokens.data();
            wparams.prompt_n_tokens  = params.no_context ? 0       : prompt_tokens.size();

            if (whisper_full(ctx, wparams, pcmf32.data(), pcmf32.size()) != 0) {
                fprintf(stderr, "failed to process audio\n");
                return 7;
            }

            // print result
            {
                if (!use_vad) {
                    printf("\33[2K\r");
                    // print long empty line to clear the previous line
                    printf("%s", std::string(100, ' ').c_str());
                    printf("\33[2K\r");
                } else {
                    const int64_t t1 = (t_last - t_start).count()/1000000;
                    const int64_t t0 = std::max(0.0, t1 - pcmf32.size()*1000.0/WHISPER_SAMPLE_RATE);

                    printf("\n");
                    printf("### Transcription %d START | t0 = %d ms | t1 = %d ms\n", n_iter, (int) t0, (int) t1);
                    printf("\n");
                }

                const int n_segments = whisper_full_n_segments(ctx);
                for (int i = 0; i < n_segments; ++i) {
                    const char * text = whisper_full_get_segment_text(ctx, i);

                    if (params.no_timestamps) {
                        printf("%s", text);
                        fflush(stdout);

                        if (params.fname_out.length() > 0) {
                            fout << text;
                        }
                    } else {
                        const int64_t t0 = whisper_full_get_segment_t0(ctx, i);
                        const int64_t t1 = whisper_full_get_segment_t1(ctx, i);

                        std::string output = "[" + to_timestamp(t0, false) + " --> " + to_timestamp(t1, false) + "]  " + text;

                        if (whisper_full_get_segment_speaker_turn_next(ctx, i)) {
                            output += " [SPEAKER_TURN]";
                        }

                        output += "\n";

                        printf("%s", output.c_str());
                        fflush(stdout);

                        if (params.fname_out.length() > 0) {
                            fout << output;
                        }
                    }
                }

                if (params.fname_out.length() > 0) {
                    fout << std::endl;
                }

                if (use_vad) {
                    printf("\n");
                    printf("### Transcription %d END\n", n_iter);
                }
            }

            ++n_iter;

            if (!use_vad && (n_iter % n_new_line) == 0) {
                printf("\n");

                // keep part of the audio for next iteration to try to mitigate word boundary issues
                pcmf32_old = std::vector<float>(pcmf32.end() - n_samples_keep, pcmf32.end());

                // Add tokens of the last full length segment as the prompt
                if (!params.no_context) {
                    prompt_tokens.clear();

                    const int n_segments = whisper_full_n_segments(ctx);
                    for (int i = 0; i < n_segments; ++i) {
                        const int token_count = whisper_full_n_tokens(ctx, i);
                        for (int j = 0; j < token_count; ++j) {
                            prompt_tokens.push_back(whisper_full_get_token_id(ctx, i, j));
                        }
                    }
                }
            }
            fflush(stdout);
        }
    }

    audio.pause();

    if (params.save_audio) {
        wavWriter.close();
    }

    whisper_print_timings(ctx);
    whisper_free(ctx);

    return 0;
}

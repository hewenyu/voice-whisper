#include "common-sdl.h"

#include <cstdio>

// the callback is called by SDL when it needs more audio data
static void audio_callback(void * userdata, uint8_t * stream, int len) {
    audio_async * audio = (audio_async *) userdata;
    audio->callback(stream, len);
}

audio_async::audio_async(int len_ms) {
    m_len_ms = len_ms;
}

audio_async::~audio_async() {
    if (m_dev_id_in) {
        SDL_CloseAudioDevice(m_dev_id_in);
    }
}

bool audio_async::init(int capture_id, int sample_rate) {
    SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't initialize SDL: %s\n", SDL_GetError());
        return false;
    }

    SDL_SetHintWithPriority(SDL_HINT_AUDIO_RESAMPLING_MODE, "medium", SDL_HINT_OVERRIDE);

    {
        int nDevices = SDL_GetNumAudioDevices(SDL_TRUE);
        fprintf(stderr, "%s: found %d capture devices:\n", __func__, nDevices);
        for (int i = 0; i < nDevices; i++) {
            fprintf(stderr, "%s:    - Capture device #%d: '%s'\n", __func__, i, SDL_GetAudioDeviceName(i, SDL_TRUE));
        }
    }

    SDL_AudioSpec capture_spec_requested;
    SDL_AudioSpec capture_spec_obtained;

    SDL_zero(capture_spec_requested);
    SDL_zero(capture_spec_obtained);

    capture_spec_requested.freq     = sample_rate;
    capture_spec_requested.format   = AUDIO_F32;
    capture_spec_requested.channels = 1;
    capture_spec_requested.samples  = 1024;
    capture_spec_requested.callback = ::audio_callback;
    capture_spec_requested.userdata = this;

    if (capture_id >= 0) {
        fprintf(stderr, "%s: attempt to open capture device %d : '%s' ...\n", __func__, capture_id, SDL_GetAudioDeviceName(capture_id, SDL_TRUE));
        m_dev_id_in = SDL_OpenAudioDevice(SDL_GetAudioDeviceName(capture_id, SDL_TRUE), SDL_TRUE, &capture_spec_requested, &capture_spec_obtained, 0);
    } else {
        fprintf(stderr, "%s: attempt to open default capture device ...\n", __func__);
        m_dev_id_in = SDL_OpenAudioDevice(nullptr, SDL_TRUE, &capture_spec_requested, &capture_spec_obtained, 0);
    }

    if (!m_dev_id_in) {
        fprintf(stderr, "%s: couldn't open an audio device for capture: %s!\n", __func__, SDL_GetError());
        m_dev_id_in = 0;

        return false;
    }

    m_sample_rate = capture_spec_obtained.freq;
    const int n_samples = (m_sample_rate*m_len_ms)/1000;

    m_audio.resize(n_samples);
    m_audio_pos = 0;
    m_audio_len = 0;

    fprintf(stderr, "%s: capture spec: freq=%d format=%d channels=%d samples=%d\n",
            __func__,
            capture_spec_obtained.freq,
            capture_spec_obtained.format,
            capture_spec_obtained.channels,
            capture_spec_obtained.samples);

    return true;
}

bool audio_async::resume() {
    if (!m_dev_id_in) {
        fprintf(stderr, "%s: no audio device to resume!\n", __func__);
        return false;
    }

    SDL_PauseAudioDevice(m_dev_id_in, 0);

    return true;
}

bool audio_async::pause() {
    if (!m_dev_id_in) {
        fprintf(stderr, "%s: no audio device to pause!\n", __func__);
        return false;
    }

    SDL_PauseAudioDevice(m_dev_id_in, 1);

    return true;
}

bool audio_async::clear() {
    if (!m_dev_id_in) {
        fprintf(stderr, "%s: no audio device to clear!\n", __func__);
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    m_audio_pos = 0;
    m_audio_len = 0;

    return true;
}

void audio_async::callback(uint8_t * stream, int len) {
    if (!m_dev_id_in) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    const size_t n_samples = len/sizeof(float);

    const float * fstream = (const float *) stream;

    if (m_audio_pos + n_samples > m_audio.size()) {
        const size_t n0 = m_audio.size() - m_audio_pos;

        memcpy(&m_audio[m_audio_pos], fstream, n0*sizeof(float));
        memcpy(&m_audio[0], &fstream[n0], (n_samples - n0)*sizeof(float));

        m_audio_pos = n_samples - n0;
    } else {
        memcpy(&m_audio[m_audio_pos], fstream, n_samples*sizeof(float));
        m_audio_pos += n_samples;
    }

    if (m_audio_pos >= m_audio.size()) {
        m_audio_pos = 0;
    }

    m_audio_len = m_audio.size();
}

void audio_async::get(int ms, std::vector<float> & result) {
    if (!m_dev_id_in) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (ms <= 0) {
        ms = m_len_ms;
    }

    const size_t n_samples = (m_sample_rate*ms)/1000;
    if (n_samples > m_audio_len) {
        result.clear();
        return;
    }

    result.resize(n_samples);

    const size_t pos = m_audio_pos > n_samples ? m_audio_pos - n_samples : m_audio.size() - (n_samples - m_audio_pos);

    if (pos + n_samples <= m_audio.size()) {
        memcpy(result.data(), &m_audio[pos], n_samples*sizeof(float));
    } else {
        const size_t n0 = m_audio.size() - pos;

        memcpy(result.data(), &m_audio[pos], n0*sizeof(float));
        memcpy(&result[n0], &m_audio[0], (n_samples - n0)*sizeof(float));
    }
}

bool sdl_poll_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                {
                    return false;
                } break;
            case SDL_KEYDOWN:
                {
                    switch (event.key.keysym.sym) {
                        case SDLK_ESCAPE:
                            {
                                return false;
                            } break;
                    }
                } break;
        }
    }

    return true;
} 
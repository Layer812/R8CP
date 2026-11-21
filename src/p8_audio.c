/*
 * p8_audio.c
 *
 *  Created on: Dec 13, 2023
 *      Author: bbaker
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
/*
 * Modified by Layer8
 * - Fixed channel_mask logic for music() so BGM plays correctly.
 * - Fixed sfx() channel auto-assignment to overwrite oldest sound when full.
 * - Added WAVEFORM_PHASER basic approximation.
 */
#include "p8_audio.h"
#include "p8_dsp.h"
#include "p8_emu.h"
#include "p8_queue.h"

#ifdef SDL
#include "SDL.h"
#endif

#define EFFECT_MASK 0x7000
#define VOLUME_MASK 0x0E00
#define WAVEFORM_MASK 0x01C0
#define PITCH_MASK 0x003F

#define EFFECT_SHIFT 12
#define VOLUME_SHIFT 9
#define WAVEFORM_SHIFT 6

#define PCM_BUFFER_SIZE 1024

enum
{
    SOUNDMODE_NONE,
    SOUNDMODE_SOUND,
    SOUNDMODE_MUSIC
};

typedef struct
{
    int pattern;
    uint8_t channel_mask;
} musicstate_t;

typedef struct
{
    int sound_mode;
    int sound_index;
    int sample;
    int position;
    int end;
} soundstate_t;

typedef struct
{
    int32_t index;
    int32_t channel;
    uint32_t start;
    uint32_t end;
} sound_t;

typedef struct
{
    int32_t index;
    int32_t fadems;
    int32_t mask;
} music_t;

typedef struct
{
    int sound_mode;

    union
    {
        sound_t sound;
        music_t music;
    };
} soundcommand_t;

enum
{
    TONE_C,
    TONE_CS,
    TONE_D,
    TONE_DS,
    TONE_E,
    TONE_F,
    TONE_FS,
    TONE_G,
    TONE_GS,
    TONE_A,
    TONE_AS,
    TONE_B
};

enum
{
    WAVEFORM_TRIANGLE,
    WAVEFORM_TILTEDSAW,
    WAVEFORM_SAW,
    WAVEFORM_SQUARE,
    WAVEFORM_PULSE,
    WAVEFORM_ORGAN,
    WAVEFORM_NOISE,
    WAVEFORM_PHASER
};

enum
{
    EFFECT_NONE,
    EFFECT_SLIDE,
    EFFECT_VIBRATO,
    EFFECT_DROP,
    EFFECT_FADEIN,
    EFFECT_FADEOUT,
    EFFECT_ARPEGGIOFAST,
    EFFECT_ARPEGGIOSLOW
};

const char *m_tone_map[] = {
    "C ", "C#", "D ", "D#", "E ", "F ", "F#", "G ", "G#", "A ", "A#", "B "};

const float m_tone_frequencies[] = {
    130.81f, 138.59f, 146.83f, 155.56f, 164.81f, 174.61f, 185.00f, 196.00f, 207.65f, 220.0f, 233.08f, 246.94f};

void render_sounds(int16_t *buffer, int total_samples);

bool m_music_enabled = true;
bool m_sound_enabled = true;

soundcommand_t m_sound_buffer[SOUND_QUEUE_SIZE];

p8_queue_t m_sound_queue = {
    .data_buf = m_sound_buffer,
    .elements_num_max = SOUND_QUEUE_SIZE,
    .elements_size = sizeof(soundcommand_t),
};

pthread_mutex_t m_sound_queue_mutex;

soundstate_t m_channels[CHANNEL_COUNT];
musicstate_t m_music_state;

uint8_t m_pcm_buffer[PCM_BUFFER_SIZE];
int m_pcm_write_pos = 0;
int m_pcm_read_pos = 0;
int m_pcm_buffered = 0;
int m_pcm_repeat = 0;
int16_t m_pcm_dampen = 0;

#ifdef SDL
SDL_AudioSpec m_audio_spec;
#endif

void audio_callback(void *userdata, uint8_t *cbuffer, int length)
{
    render_sounds((int16_t *)cbuffer, length / sizeof(int16_t));
}

void audio_init()
{
    pthread_mutex_init(&m_sound_queue_mutex, NULL);
    _queue_init(&m_sound_queue);

#ifdef SDL
    m_audio_spec.freq = SAMPLE_RATE;
    m_audio_spec.format = AUDIO_S16SYS;
    m_audio_spec.channels = 1;
    m_audio_spec.samples = SOUND_BUFFER_SIZE;
    m_audio_spec.userdata = NULL;
    m_audio_spec.callback = audio_callback;

    int ret = SDL_OpenAudio(&m_audio_spec, &m_audio_spec);

    if (ret != 0)
    {
        printf("Error on SDL_OpenAudio()\n");
    }

    SDL_PauseAudio(0);
#endif
}

void audio_resume()
{
#ifdef SDL
    SDL_PauseAudio(0);
#endif
}

void audio_pause()
{
#ifdef SDL
    SDL_PauseAudio(1);
#endif
}

void audio_close()
{
#ifdef SDL
    SDL_CloseAudio();
#endif
}

void audio_sound(int32_t index, int32_t channel, uint32_t start, uint32_t end)
{
    soundcommand_t sound_command;
    sound_command.sound_mode = SOUNDMODE_SOUND;
    sound_command.sound.index = index;
    sound_command.sound.channel = channel;
    sound_command.sound.start = start;
    sound_command.sound.end = end;

    pthread_mutex_lock(&m_sound_queue_mutex);
    queue_add_back(&m_sound_queue, &sound_command);
    pthread_mutex_unlock(&m_sound_queue_mutex);
}

void audio_music(int32_t index, int32_t fadems, int32_t mask)
{
    soundcommand_t sound_command;
    sound_command.sound_mode = SOUNDMODE_MUSIC;
    sound_command.music.index = index;
    sound_command.music.fadems = fadems;
    sound_command.music.mask = mask;

    pthread_mutex_lock(&m_sound_queue_mutex);
    queue_add_back(&m_sound_queue, &sound_command);
    pthread_mutex_unlock(&m_sound_queue_mutex);
}

int32_t audio_stat(int32_t index)
{
    if (index >= 16 && index <= 19)
    {
        int channel = index - 16;
        if (m_channels[channel].sound_mode == SOUNDMODE_NONE)
            return -1;
        return m_channels[channel].sound_index;
    }
    if (index >= 20 && index <= 23) {
        int channel = index - 20;
        if (m_channels[channel].sound_mode == SOUNDMODE_NONE)
            return -1;
        return m_channels[channel].position;
    }
    if (index >= 46 && index <= 49) {
        int channel = index - 46;
        if (m_channels[channel].sound_mode == SOUNDMODE_NONE)
            return -1;
        return m_channels[channel].sound_index;
    }
    if (index >= 50 && index <= 53) {
        int channel = index - 50;
        if (m_channels[channel].sound_mode == SOUNDMODE_NONE)
            return -1;
        return m_channels[channel].position;
    }
    if (index == 24 || index == 54) {
        bool any_music = false;
        for (int i = 0; i < CHANNEL_COUNT; ++i) {
            if (m_channels[i].sound_mode == SOUNDMODE_MUSIC) {
                any_music = true;
                break;
            }
        }
        return any_music ? m_music_state.pattern : -1;
    }
    if (index == 25 || index == 55) {
        bool any_music = false;
        for (int i = 0; i < CHANNEL_COUNT; ++i) {
            if (m_channels[i].sound_mode == SOUNDMODE_MUSIC) {
                any_music = true;
                break;
            }
        }
        return any_music ? m_music_state.pattern : -1;
    }
    if (index == 26 || index == 56) {
        int ticks = 0;
        for (int i = 0; i < CHANNEL_COUNT; ++i)
            if (m_channels[i].sound_mode == SOUNDMODE_MUSIC)
                ticks = MAX(ticks, m_channels[i].sample);
        return ticks;
    }
    if (index == 57) {
        for (int i = 0; i < CHANNEL_COUNT; ++i) {
            if (m_channels[i].sound_mode == SOUNDMODE_MUSIC)
                return 1;
        }
        return 0;
    }
    return 0;
}

void update_channel(soundstate_t *channel)
{
    if (channel->sound_mode == SOUNDMODE_NONE)
        return;

    if (channel->sound_mode == SOUNDMODE_MUSIC)
    {
        if (channel->sample >= channel->end)
        {
            bool is_loop_begin = m_memory[MEMORY_MUSIC + 4 * m_music_state.pattern] & (1 << 7);
            bool is_loop_end = m_memory[MEMORY_MUSIC + 4 * m_music_state.pattern + 1] & (1 << 7);
            bool is_stop = m_memory[MEMORY_MUSIC + 4 * m_music_state.pattern + 2] & (1 << 7);
            if (is_stop)
            {
                for (int j = 0; j < CHANNEL_COUNT; j++)
                {
                    m_channels[j].sound_mode = SOUNDMODE_NONE;
                    m_channels[j].sample = 0;
                    m_channels[j].position = 0;
                    m_channels[j].end = 0;
                }
                return;
            }
            m_music_state.pattern++;
            if (is_loop_end || m_music_state.pattern == MUSIC_COUNT)
            {
                int i = m_music_state.pattern - 1;
                while (i >= 0)
                {
                    is_loop_begin = (m_memory[MEMORY_MUSIC + 4 * i] & (1 << 7));
                    if (is_loop_begin || i == 0)
                        break;
                    i--;
                }
                m_music_state.pattern = i;
            }
            for (int i = 0; i < CHANNEL_COUNT; i++)
            {
                uint8_t channel_data = m_memory[MEMORY_MUSIC + 4 * m_music_state.pattern + i];
                bool channel_reserved = (m_music_state.channel_mask == 0) || ((m_music_state.channel_mask & (1 << i)) != 0);
                bool enabled = (channel_data & (1 << 6)) == 0 && channel_reserved;
                if (enabled)
                {
                    m_channels[i].sound_mode = SOUNDMODE_MUSIC;
                    m_channels[i].sound_index = channel_data & 0x7F;
                    m_channels[i].sample = 0;
                    m_channels[i].position = 0;
                    m_channels[i].end = 32;
                }
                else
                    m_channels[i].sound_mode = SOUNDMODE_NONE;
            }
        }
    }
    else if (channel->sound_mode == SOUNDMODE_SOUND)
    {
        if (channel->sample >= channel->end)
            channel->sound_mode = SOUNDMODE_NONE;
    }
}

void update_sound_queue()
{
    pthread_mutex_lock(&m_sound_queue_mutex);

    soundcommand_t sound_command;

    while (queue_get_front(&m_sound_queue, &sound_command))
    {
        if (sound_command.sound_mode == SOUNDMODE_SOUND)
        {
            sound_t *sound = &sound_command.sound;

            if (sound->index == -1)
            {
                if (sound->channel >= 0 && sound->channel < CHANNEL_COUNT)
                    m_channels[sound->channel].sound_mode = SOUNDMODE_NONE;
                continue;
            }
            else if (sound->index == -2)
                continue;
            else if (sound->channel == -2)
            {
                for (int i = 0; i < CHANNEL_COUNT; i++)
                {
                    soundstate_t *channel = &m_channels[i];

                    if (channel->sound_index == sound->index)
                        channel->sound_mode = SOUNDMODE_NONE;
                }
                continue;
            }
            else if (sound->channel == -1)
            {
                int best_channel = -1;
                // 1st pass: find completely empty channel
                for (int i = 0; i < CHANNEL_COUNT; i++)
                {
                    if (m_channels[i].sound_mode == SOUNDMODE_NONE)
                    {
                        best_channel = i;
                        break;
                    }
                }
                
                // 2nd pass: if no empty channel, find the oldest SOUNDMODE_SOUND to overwrite
                if (best_channel == -1)
                {
                    int oldest_sample = -1;
                    for (int i = 0; i < CHANNEL_COUNT; i++)
                    {
                        if (m_channels[i].sound_mode == SOUNDMODE_SOUND)
                        {
                            if (m_channels[i].sample > oldest_sample)
                            {
                                oldest_sample = m_channels[i].sample;
                                best_channel = i;
                            }
                        }
                    }
                }
                
                sound->channel = best_channel;
            }
            if (sound->channel >= 0 && sound->channel < CHANNEL_COUNT && sound->index >= 0 && sound->index < SOUND_COUNT)
            {
                soundstate_t *channel = &m_channels[sound->channel];
                uint8_t speed = m_memory[MEMORY_SFX + 68 * sound->index + 64 + 1];
                int sample_per_tick = (SAMPLE_RATE / 128) * (speed + 1);
                channel->sound_mode = SOUNDMODE_SOUND;
                channel->sound_index = sound->index;
                channel->end = sound->start + sound->end;
                channel->sample = sound->start;
                channel->position = sound->start * sample_per_tick;
            }
        }
        else if (sound_command.sound_mode == SOUNDMODE_MUSIC)
        {
            music_t *music = &sound_command.music;

            if (music->index == -1)
            {
                for (int i = 0; i < CHANNEL_COUNT; i++)
                    m_channels[i].sound_mode = SOUNDMODE_NONE;
            }
            else
            {
                m_music_state.pattern = music->index;
                m_music_state.channel_mask = music->mask;
#ifdef IS_CARDPUTER
                printf("[Audio] Starting music pattern: %d, mask: %d\n", music->index, music->mask);
#endif
                for (int i = 0; i < CHANNEL_COUNT; i++)
                {
                    uint8_t channel_data = m_memory[MEMORY_MUSIC + 4 * m_music_state.pattern + i];
                    bool channel_reserved = (m_music_state.channel_mask == 0) || ((m_music_state.channel_mask & (1 << i)) != 0);
                    bool enabled = (channel_data & (1 << 6)) == 0 && channel_reserved;
                    if (enabled)
                    {
                        m_channels[i].sound_mode = SOUNDMODE_MUSIC;
                        m_channels[i].sound_index = channel_data & 0x7F;
                        m_channels[i].sample = 0;
                        m_channels[i].position = 0;
                        m_channels[i].end = 32;
#ifdef IS_CARDPUTER
                        printf("[Audio] Music channel %d enabled, sound_index: %d\n", i, channel_data & 0x7F);
#endif
                    }
                    else
                    {
                        m_channels[i].sound_mode = SOUNDMODE_NONE;
                    }
                }
            }
        }
    }

    pthread_mutex_unlock(&m_sound_queue_mutex);
}

float get_frequency(int pitch)
{
    return m_tone_frequencies[pitch % 12] / 2 * (1 << (pitch / 12));
}

void render_sound(int waveform, int pitch, int volume, int position, int offset, int length, int16_t *buffer)
{
    int16_t amplitude = (int16_t)((MAX_VOLUME / 8) * volume);
    unsigned int frequency = (unsigned int)get_frequency(pitch);
    switch (waveform)
    {
    case WAVEFORM_TRIANGLE:
        dsp_triangle_wave(frequency, amplitude, 0, position, offset, length, buffer);
        break;
    case WAVEFORM_TILTEDSAW:
        dsp_tilted_sawtooth_wave(frequency, amplitude, 0, 0.85f, position, offset, length, buffer);
        break;
    case WAVEFORM_SAW:
        dsp_sawtooth_wave(frequency, amplitude, 0, position, offset, length, buffer);
        break;
    case WAVEFORM_SQUARE:
        dsp_square_wave(frequency, amplitude, 0, position, offset, length, buffer);
        break;
    case WAVEFORM_PULSE:
        dsp_pulse_wave(frequency, amplitude, 0, 1.0f / 3.0f, position, offset, length, buffer);
        break;
    case WAVEFORM_ORGAN:
        dsp_organ_wave(frequency, amplitude, 0, 0.5f, position, offset, length, buffer);
        break;
    case WAVEFORM_NOISE:
        dsp_noise(frequency, amplitude, position, offset, length, buffer);
        break;
    case WAVEFORM_PHASER:
        // A simple approximation of phaser: two triangle waves slightly detuned
        dsp_triangle_wave(frequency, amplitude / 2, 0, position, offset, length, buffer);
        dsp_triangle_wave((unsigned int)(frequency * 1.01f), amplitude / 2, 0, position, offset, length, buffer);
        break;
    }
}

static inline int16_t r8cp_audio_sat16_pcm(int32_t value)
{
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

void render_sounds(int16_t *buffer, int total_samples)
{
    update_sound_queue();

    memset(buffer, 0, sizeof(int16_t) * total_samples);

    // 0x5f2f == 1: audio engine is paused
    if (m_memory[MEMORY_AUDIO_PAUSE] == 1)
        return;

    int index = 0;

    while (index < total_samples)
    {
        uint8_t music_speed = 16;
        for (int i = 0; i < CHANNEL_COUNT; i++) {
            if (m_channels[i].sound_mode == SOUNDMODE_MUSIC) {
                music_speed = m_memory[MEMORY_SFX + 68 * m_channels[i].sound_index + 64 + 1];
                break;
            }
        }

        int chunk_length = total_samples - index;
        for (int i = 0; i < CHANNEL_COUNT; i++)
        {
            soundstate_t *channel = &m_channels[i];
            if (channel->sound_mode != SOUNDMODE_NONE)
            {
                if (channel->sample >= channel->end)
                    continue;

                uint8_t speed = (channel->sound_mode == SOUNDMODE_MUSIC) ? music_speed :
                    m_memory[MEMORY_SFX + 68 * channel->sound_index + 64 + 1];
                int sample_per_tick = (SAMPLE_RATE / 128) * (speed + 1);
                int samples_to_next_tick = sample_per_tick - (channel->position % sample_per_tick);
                if (samples_to_next_tick < chunk_length)
                    chunk_length = samples_to_next_tick;
            }
        }

        if (chunk_length <= 0)
            chunk_length = 1;

        for (int i = 0; i < CHANNEL_COUNT; i++)
        {
            soundstate_t *channel = &m_channels[i];

            if ((channel->sound_mode == SOUNDMODE_MUSIC && m_music_enabled) ||
                (channel->sound_mode == SOUNDMODE_SOUND && m_sound_enabled))
            {
                if (channel->sample >= channel->end)
                    continue;

                uint8_t speed = (channel->sound_mode == SOUNDMODE_MUSIC) ? music_speed :
                    m_memory[MEMORY_SFX + 68 * channel->sound_index + 64 + 1];
                int sample_per_tick = (SAMPLE_RATE / 128) * (speed + 1);

                uint8_t data_lo = m_memory[MEMORY_SFX + 68 * channel->sound_index + channel->sample * 2];
                uint8_t data_hi = m_memory[MEMORY_SFX + 68 * channel->sound_index + channel->sample * 2 + 1];
                uint16_t data = (uint16_t)((data_hi << 8) | data_lo);

                uint8_t effect = (data & EFFECT_MASK) >> EFFECT_SHIFT;
                uint8_t volume = (data & VOLUME_MASK) >> VOLUME_SHIFT;
                uint8_t waveform = (data & WAVEFORM_MASK) >> WAVEFORM_SHIFT;
                uint8_t pitch = data & PITCH_MASK;

                int eff_pitch = pitch;
                int eff_volume = volume;

                if (effect != EFFECT_NONE)
                {
                    float t = (float)(channel->position % sample_per_tick) / (float)sample_per_tick;
                    switch (effect)
                    {
                    case EFFECT_SLIDE:
                    {
                        int prev_pitch = pitch;
                        if (channel->sample > 0)
                        {
                            uint8_t plo = m_memory[MEMORY_SFX + 68 * channel->sound_index + (channel->sample - 1) * 2];
                            uint8_t phi = m_memory[MEMORY_SFX + 68 * channel->sound_index + (channel->sample - 1) * 2 + 1];
                            prev_pitch = ((uint16_t)((phi << 8) | plo)) & PITCH_MASK;
                        }
                        eff_pitch = (int)(prev_pitch + (pitch - prev_pitch) * t);
                    }
                    break;
                    case EFFECT_VIBRATO:
                        eff_pitch = pitch + (int)(sinf(t * 2.0f * PI) * 1.0f);
                        break;
                    case EFFECT_DROP:
                        eff_pitch = (int)(pitch * (1.0f - t));
                        break;
                    case EFFECT_FADEIN:
                        eff_volume = (int)(volume * t);
                        break;
                    case EFFECT_FADEOUT:
                        eff_volume = (int)(volume * (1.0f - t));
                        break;
                    case EFFECT_ARPEGGIOFAST:
                    {
                        int phase = (int)(t * 4.0f) % 3;
                        if (phase == 1) eff_pitch = pitch + 4;
                        else if (phase == 2) eff_pitch = pitch + 7;
                    }
                    break;
                    case EFFECT_ARPEGGIOSLOW:
                    {
                        int phase = (int)(t * 2.0f) % 3;
                        if (phase == 1) eff_pitch = pitch + 4;
                        else if (phase == 2) eff_pitch = pitch + 7;
                    }
                    break;
                    }
                    if (eff_pitch < 0) eff_pitch = 0;
                    if (eff_pitch > 63) eff_pitch = 63;
                    if (eff_volume < 0) eff_volume = 0;
                    if (eff_volume > 7) eff_volume = 7;
                }

                render_sound(waveform, eff_pitch, eff_volume, channel->position, index, chunk_length, buffer);

                channel->position += chunk_length;
                channel->sample = channel->position / sample_per_tick;
            }
        }

        index += chunk_length;

        bool music_advance = false;
        bool has_music = false;
        bool all_music_ended = true;
        for (int i = 0; i < CHANNEL_COUNT; i++)
        {
            soundstate_t *channel = &m_channels[i];
            if (channel->sound_mode == SOUNDMODE_MUSIC)
            {
                has_music = true;
                if (channel->sample < channel->end)
                    all_music_ended = false;
            }
            else if (channel->sound_mode == SOUNDMODE_SOUND)
            {
                if (channel->sample >= channel->end)
                    channel->sound_mode = SOUNDMODE_NONE;
            }
        }

        if (has_music && all_music_ended)
        {
            bool is_loop_begin = m_memory[MEMORY_MUSIC + 4 * m_music_state.pattern] & (1 << 7);
            bool is_loop_end = m_memory[MEMORY_MUSIC + 4 * m_music_state.pattern + 1] & (1 << 7);
            bool is_stop = m_memory[MEMORY_MUSIC + 4 * m_music_state.pattern + 2] & (1 << 7);

            if (is_stop)
            {
                for (int j = 0; j < CHANNEL_COUNT; j++)
                    m_channels[j].sound_mode = SOUNDMODE_NONE;
            }
            else
            {
                m_music_state.pattern++;
                if (is_loop_end || m_music_state.pattern == MUSIC_COUNT)
                {
                    int i = m_music_state.pattern - 1;
                    while (i >= 0)
                    {
                        is_loop_begin = (m_memory[MEMORY_MUSIC + 4 * i] & (1 << 7));
                        if (is_loop_begin || i == 0)
                            break;
                        i--;
                    }
                    m_music_state.pattern = i;
                }

                for (int i = 0; i < CHANNEL_COUNT; i++)
                {
                    uint8_t channel_data = m_memory[MEMORY_MUSIC + 4 * m_music_state.pattern + i];
                    bool channel_reserved = (m_music_state.channel_mask == 0) || ((m_music_state.channel_mask & (1 << i)) != 0);
                    bool enabled = (channel_data & (1 << 6)) == 0 && channel_reserved;
                    if (enabled)
                    {
                        m_channels[i].sound_mode = SOUNDMODE_MUSIC;
                        m_channels[i].sound_index = channel_data & 0x7F;
                        m_channels[i].sample = 0;
                        m_channels[i].position = 0;
                        m_channels[i].end = 32;
                    }
                    else
                    {
                        m_channels[i].sound_mode = SOUNDMODE_NONE;
                    }
                }
            }
        }
    }

    const bool dampen_enabled = (m_memory[MEMORY_MISCFLAGS] & 0x20) == 0;

    for (int i = 0; i < total_samples; i++)
    {
        if (m_pcm_buffered > 0)
        {
            uint8_t pcm_sample = m_pcm_buffer[m_pcm_read_pos];
            int16_t sample16 = (int16_t)((pcm_sample - 128) * 256);

            if (dampen_enabled)
            {
                m_pcm_dampen = (sample16 + m_pcm_dampen * 3) / 4;
                buffer[i] = r8cp_audio_sat16_pcm((int32_t)buffer[i] + m_pcm_dampen);
            }
            else
            {
                buffer[i] = r8cp_audio_sat16_pcm((int32_t)buffer[i] + sample16);
            }

            m_pcm_repeat++;
            if (m_pcm_repeat >= 8)
            {
                m_pcm_repeat = 0;
                m_pcm_read_pos = (m_pcm_read_pos + 1) % PCM_BUFFER_SIZE;
                m_pcm_buffered--;
            }
        }
        else
        {
            m_pcm_repeat = 0;
            m_pcm_dampen = 0;
        }
    }
}

void audio_pcm_write(uint16_t address, uint16_t length)
{
#ifdef ENABLE_AUDIO
    if (address >= MEMORY_SIZE || length == 0)
        return;

    if (address + length > MEMORY_SIZE)
        length = MEMORY_SIZE - address;

    if (length > PCM_BUFFER_SIZE - m_pcm_buffered)
        length = PCM_BUFFER_SIZE - m_pcm_buffered;

    for (uint32_t i = 0; i < length; i++)
    {
        m_pcm_buffer[m_pcm_write_pos] = m_memory[address + i];
        m_pcm_write_pos = (m_pcm_write_pos + 1) % PCM_BUFFER_SIZE;
        m_pcm_buffered++;
    }
#endif
}

int16_t audio_pcm_buffered()
{
#ifdef ENABLE_AUDIO
    return m_pcm_buffered;
#else
    return 0;
#endif
}

int16_t audio_pcm_app_buffer()
{
#ifdef ENABLE_AUDIO
    return PCM_BUFFER_SIZE / 4;
#else
    return 0;
#endif
}

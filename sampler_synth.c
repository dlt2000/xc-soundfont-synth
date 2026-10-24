// SPDX-License-Identifier: MIT
// Copyright (c) 2026 xc-soundfont-synth contributors
#include "sampler_synth.h"

#include <math.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TSF_IMPLEMENTATION
#define TSF_RENDER_EFFECTSAMPLEBLOCK 64
#define TSF_RENDER_SHORTBUFFERBLOCK 128
#include "tsf.h"

#define SYNTH_OUTPUT_CHANNELS 2
#define SYNTH_DEFAULT_RATE 44100U
#define SYNTH_DEFAULT_CHUNK_FRAMES 128U
#define SYNTH_DEFAULT_MAX_VOICES 8U
#define SYNTH_COMMAND_QUEUE_LENGTH 16U
#define SYNTH_TASK_STACK_SIZE 6144U
#define SYNTH_TASK_PRIORITY 8U
#define SYNTH_CHANNEL_COUNT 2U
#define SYNTH_PRIMARY_CHANNEL 0U
#define SYNTH_SECONDARY_CHANNEL 1U
#define SYNTH_EXPRESSION_SMOOTH_MS 5U
#define SYNTH_LEGATO_TRANSITION_MS 5U
#define SYNTH_CONTROL_BLOCK_FRAMES 64U
#define SYNTH_SHUTDOWN_TIMEOUT_MS 1000U
#define SYNTH_DIAGNOSTIC_PERIOD_US (10LL * 1000LL * 1000LL)

typedef enum
{
    SYNTH_CMD_NOTE_ON = 0,
    SYNTH_CMD_NOTE_OFF,
    SYNTH_CMD_LEGATO,
    SYNTH_CMD_LOAD_TIMBRE,
    SYNTH_CMD_SET_REVERB,
    SYNTH_CMD_SHUTDOWN,
} synth_command_type_t;

typedef enum
{
    SYNTH_LEGATO_FADE_OUT = 0,
    SYNTH_LEGATO_FADE_IN,
} synth_legato_phase_t;

typedef struct
{
    synth_command_type_t type;
    int note;
    int previous_note;
    uint8_t velocity;
    uint8_t timbre_index;
    bool enabled;
    uint32_t trace_seq;
    int64_t audio_call_ts_us;
    char path[128];
} synth_command_t;

typedef struct
{
    bool enabled;
    float wet;
    float feedback;
    float damp;
    float last_l;
    float last_r;
    float *buffer_l;
    float *buffer_r;
    size_t delay_l;
    size_t delay_r;
    size_t index_l;
    size_t index_r;
} synth_reverb_t;

typedef struct
{
    sampler_synth_cfg_t cfg;
    int16_t *output; /* init 分配，渲染任务退出后才释放。 */
    tsf *font;
    int preset_index;
    QueueHandle_t command_queue;
    SemaphoreHandle_t stopped_sem;
    SemaphoreHandle_t shutdown_sem;
    TaskHandle_t task;
    atomic_bool running; /* 生命周期调用方写入，渲染任务与命令生产者读取。 */
    volatile bool shutdown_frame_pending;
    volatile bool shutdown_frame_ok;
    volatile uint8_t expression_target;
    float expression_current;
    uint8_t expression_ramp_target;
    uint32_t expression_frames_remaining;
    uint8_t expression_applied[SYNTH_CHANNEL_COUNT];
    float channel_gain[SYNTH_CHANNEL_COUNT];
    uint32_t legato_frames_remaining;
    uint32_t legato_total_frames;
    int active_note;
    int legato_old_note;
    int legato_new_note;
    uint8_t active_channel;
    uint8_t legato_old_channel;
    uint8_t legato_new_channel;
    bool legato_active;
    synth_legato_phase_t legato_phase;
    uint8_t legato_velocity;
    uint32_t legato_trace_seq;
    bool deferred_legato_valid;
    int deferred_legato_note;
    uint8_t deferred_legato_velocity;
    uint32_t deferred_legato_trace_seq;
    int64_t deferred_legato_call_ts_us;
    volatile sampler_synth_state_t state;
    volatile sampler_synth_error_t last_error;
    volatile int loaded_timbre;
    portMUX_TYPE status_lock;
    synth_reverb_t reverb;
    uint32_t max_render_us;
    uint32_t max_render_steady_us;
    uint32_t max_render_legato_us;
    uint32_t max_output_write_us;
    uint32_t deadline_misses;
    uint32_t window_deadline_misses;
    uint32_t window_legato_misses;
    uint32_t output_stalls;
    uint32_t output_errors;
    uint32_t clipped_samples;
    uint32_t max_sample_step;
    int16_t previous_output[SYNTH_OUTPUT_CHANNELS];
    bool previous_output_valid;
    uint32_t output_analysis_frames_remaining;
    uint32_t deferred_legato_updates;
    int64_t last_diagnostic_us;
    uint32_t pending_trace_seq;
} synth_context_t;

static const char *TAG = "xc_soundfont_synth";
static synth_context_t s_synth = {
    .preset_index = -1,
    .loaded_timbre = -1,
    .status_lock = portMUX_INITIALIZER_UNLOCKED,
};
static sampler_synth_trace_cb_t s_trace_callback;

/**
 * @brief 将应用层的线性响度转换为 TinySoundFont 使用的 MIDI CC11 数值。
 *
 * TinySoundFont 会在处理 CC11 时把归一化 Expression 做三次方。如果直接把
 * 线性响度原值发送过去，中低气息会再次被大幅压低。这里预先做立方根补偿，
 * 使 TinySoundFont 三次方处理后的最终增益重新接近应用层期望的线性响度。
 *
 * @param linear_expression 应用层期望的线性响度，范围 0.0-127.0。
 * @return 补偿后的 MIDI CC11 数值，范围 0-127。
 */
static uint8_t linear_expression_to_cc11(float linear_expression)
{
    if (linear_expression <= 0.0f)
        return 0;
    if (linear_expression >= 127.0f)
        return 127;

    const float normalized_gain = linear_expression / 127.0f;
    return (uint8_t)lroundf(cbrtf(normalized_gain) * 127.0f);
}

static int16_t clamp16(int32_t value)
{
    if (value > INT16_MAX)
        return INT16_MAX;
    if (value < INT16_MIN)
        return INT16_MIN;
    return (int16_t)value;
}

static void set_status(sampler_synth_state_t state, sampler_synth_error_t error, int loaded_timbre)
{
    portENTER_CRITICAL(&s_synth.status_lock);
    s_synth.state = state;
    s_synth.last_error = error;
    s_synth.loaded_timbre = loaded_timbre;
    portEXIT_CRITICAL(&s_synth.status_lock);
}

static bool enqueue_command(const synth_command_t *command)
{
    if (!command || !s_synth.command_queue || !s_synth.running)
    {
        return false;
    }
    if (xQueueSend(s_synth.command_queue, command, 0) != pdTRUE)
    {
        portENTER_CRITICAL(&s_synth.status_lock);
        s_synth.last_error = SAMPLER_SYNTH_ERROR_QUEUE_FULL;
        portEXIT_CRITICAL(&s_synth.status_lock);
        /* 高频控制路径只返回错误；避免队列压力下日志进一步拖慢渲染。 */
        return false;
    }
    return true;
}

/**
 * @brief 重置单音演奏和双通道串行过渡状态，不访问 TinySoundFont 对象。
 */
static void reset_performance_state(void)
{
    s_synth.active_note = -1;
    s_synth.legato_old_note = -1;
    s_synth.legato_new_note = -1;
    s_synth.active_channel = SYNTH_PRIMARY_CHANNEL;
    s_synth.legato_old_channel = SYNTH_PRIMARY_CHANNEL;
    s_synth.legato_new_channel = SYNTH_SECONDARY_CHANNEL;
    s_synth.legato_active = false;
    s_synth.legato_phase = SYNTH_LEGATO_FADE_OUT;
    s_synth.legato_velocity = 0;
    s_synth.legato_trace_seq = 0;
    s_synth.deferred_legato_valid = false;
    s_synth.deferred_legato_note = -1;
    s_synth.deferred_legato_velocity = 0;
    s_synth.deferred_legato_trace_seq = 0;
    s_synth.deferred_legato_call_ts_us = 0;
    s_synth.legato_frames_remaining = 0;
    s_synth.legato_total_frames = 0;
    /* 无音状态两个后级混音增益都必须为零，避免释放 voice 在失效保音后重新漏出。 */
    s_synth.channel_gain[SYNTH_PRIMARY_CHANNEL] = 0.0f;
    s_synth.channel_gain[SYNTH_SECONDARY_CHANNEL] = 0.0f;
    for (uint8_t channel = 0; channel < SYNTH_CHANNEL_COUNT; ++channel)
    {
        s_synth.expression_applied[channel] = UINT8_MAX;
    }
}

/**
 * @brief 结束全部通道声音并清除尚未完成的 legato 过渡。
 *
 * @param immediate true 使用 all-sounds-off 立即回收 voice；false 进入 SF2 release。
 */
static void stop_all_channel_sounds(bool immediate)
{
    if (s_synth.font)
    {
        for (uint8_t channel = 0; channel < SYNTH_CHANNEL_COUNT; ++channel)
        {
            if (immediate)
                tsf_channel_sounds_off_all(s_synth.font, channel);
            else
                tsf_channel_note_off_all(s_synth.font, channel);
        }
    }
    reset_performance_state();
}

static void destroy_font(void)
{
    if (s_synth.font)
    {
        stop_all_channel_sounds(true);
        tsf_close(s_synth.font);
        s_synth.font = NULL;
    }
    s_synth.preset_index = -1;
    s_synth.expression_current = 0.0f;
    s_synth.expression_ramp_target = 0;
    s_synth.expression_frames_remaining = 0;
    reset_performance_state();
}

static int matching_zone_count(const tsf *font, int preset_index, int midi_note, int velocity)
{
    if (!font || preset_index < 0 || preset_index >= font->presetNum)
        return 0;

    int count = 0;
    const struct tsf_preset *preset = &font->presets[preset_index];
    const struct tsf_region *region = preset->regions;
    const struct tsf_region *end = region + preset->regionNum;
    for (; region != end; ++region)
    {
        if (midi_note >= region->lokey && midi_note <= region->hikey &&
            velocity >= region->lovel && velocity <= region->hivel)
        {
            ++count;
        }
    }
    return count;
}

/**
 * @brief 按调用方选择的音域和力度检查 zone；默认不要求全音域覆盖。
 * @param font 已解析音色；preset_index 为所选 bank/program 对应的内部索引。
 * @return true 表示检查关闭，或各音符至少一层且不超过配置上限。
 * 检查只在加载阶段执行，不参与实时渲染；它不证明所有力度都有可用 zone。
 */
static bool validate_delivery_zones(const tsf *font, int preset_index)
{
    if (!s_synth.cfg.validate_zones)
        return true;
    for (int note = s_synth.cfg.note_min;
         note <= s_synth.cfg.note_max;
         ++note)
    {
        const int count = matching_zone_count(font, preset_index, note, s_synth.cfg.zone_velocity);
        if (count < 1 || (s_synth.cfg.max_zones_per_note != 0 &&
                          count > s_synth.cfg.max_zones_per_note))
        {
            ESP_LOGE(TAG, "SF2 zone check failed: note=%d velocity=%u matches=%d",
                     note, s_synth.cfg.zone_velocity, count);
            return false;
        }
    }
    return true;
}

/**
 * @brief 验证 VFS 普通文件及调用方资源门槛，不挂载文件系统或强制要求 PSRAM。
 * @param path 稳定可读的路径；file_size 接收文件大小。
 * @return 文件/大小/PSRAM 门槛错误，或 NONE；实际分配和解析仍可能失败。
 */
static sampler_synth_error_t validate_soundfont_file(const char *path, size_t *file_size)
{
    struct stat info;
    if (!path || !file_size)
        return SAMPLER_SYNTH_ERROR_INVALID_ARGUMENT;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode))
        return SAMPLER_SYNTH_ERROR_FILE_OPEN;
    if (info.st_size <= 0 || (uintmax_t)info.st_size > s_synth.cfg.max_sf2_bytes)
        return SAMPLER_SYNTH_ERROR_FILE_SIZE;

    *file_size = (size_t)info.st_size;
    if (s_synth.cfg.min_free_psram_bytes != 0 &&
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < s_synth.cfg.min_free_psram_bytes)
        return SAMPLER_SYNTH_ERROR_PSRAM;
    return SAMPLER_SYNTH_ERROR_NONE;
}

/**
 * @brief 在唯一渲染任务中替换音色，应用调用方的 bank/preset 与可选 zone 检查。
 * @param timbre_index 应用标签；path 为命令内已复制的 VFS 路径。
 * @return true 表示可渲染；失败释放新音色并保持静音，不恢复旧音色。
 * 文件读取和解析会暂停输出提交，应在非演奏期调用；它不是无缝音色切换。
 */
static bool load_soundfont_now(uint8_t timbre_index, const char *path)
{
    size_t file_size = 0;
    size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int64_t start_us = esp_timer_get_time();

    set_status(SAMPLER_SYNTH_STATE_LOADING, SAMPLER_SYNTH_ERROR_NONE, -1);
    destroy_font();

    sampler_synth_error_t error = validate_soundfont_file(path, &file_size);
    if (error != SAMPLER_SYNTH_ERROR_NONE)
    {
        set_status(SAMPLER_SYNTH_STATE_ERROR, error, -1);
        ESP_LOGE(TAG, "SF2 rejected: path=%s error=%s size=%u psram_free=%u",
                 path ? path : "(null)", sampler_synth_error_name(error),
                 (unsigned)file_size, (unsigned)psram_before);
        return false;
    }

    tsf *font = tsf_load_filename(path);
    if (!font)
    {
        set_status(SAMPLER_SYNTH_STATE_ERROR, SAMPLER_SYNTH_ERROR_PARSE, -1);
        ESP_LOGE(TAG, "SF2 parse failed: %s", path);
        return false;
    }

    int preset_index = tsf_get_presetindex(font, s_synth.cfg.bank, s_synth.cfg.preset);
    if (preset_index < 0)
    {
        tsf_close(font);
        set_status(SAMPLER_SYNTH_STATE_ERROR, SAMPLER_SYNTH_ERROR_PRESET, -1);
        ESP_LOGE(TAG, "SF2 has no bank %u / preset %u: %s",
                 s_synth.cfg.bank, s_synth.cfg.preset, path);
        return false;
    }
    if (!validate_delivery_zones(font, preset_index))
    {
        tsf_close(font);
        set_status(SAMPLER_SYNTH_STATE_ERROR, SAMPLER_SYNTH_ERROR_ZONE, -1);
        return false;
    }
    if (!tsf_set_max_voices(font, s_synth.cfg.max_voices))
    {
        tsf_close(font);
        set_status(SAMPLER_SYNTH_STATE_ERROR, SAMPLER_SYNTH_ERROR_VOICE_ALLOC, -1);
        ESP_LOGE(TAG, "SF2 voice pre-allocation failed: %s", path);
        return false;
    }

    tsf_set_output(font, TSF_STEREO_INTERLEAVED, (int)s_synth.cfg.sample_rate, 0.0f);
    for (uint8_t channel = 0; channel < SYNTH_CHANNEL_COUNT; ++channel)
    {
        /* 在渲染任务正式使用前创建两个通道，避免连音换指时触发 realloc。 */
        if (!tsf_channel_set_presetindex(font, channel, preset_index))
        {
            tsf_close(font);
            set_status(SAMPLER_SYNTH_STATE_ERROR, SAMPLER_SYNTH_ERROR_PRESET, -1);
            return false;
        }
        tsf_channel_midi_control(font, channel, 11, 0);
        tsf_channel_set_mix_volume(font, channel,
                                   channel == SYNTH_PRIMARY_CHANNEL ? 1.0f : 0.0f);
    }

    s_synth.font = font;
    s_synth.preset_index = preset_index;
    s_synth.expression_current = 0.0f;
    s_synth.expression_ramp_target = 0;
    s_synth.expression_frames_remaining = 0;
    reset_performance_state();
    s_synth.channel_gain[SYNTH_PRIMARY_CHANNEL] = 1.0f;
    s_synth.deferred_legato_updates = 0;
    s_synth.expression_applied[SYNTH_PRIMARY_CHANNEL] = 0;
    s_synth.expression_applied[SYNTH_SECONDARY_CHANNEL] = 0;
    set_status(SAMPLER_SYNTH_STATE_READY, SAMPLER_SYNTH_ERROR_NONE, timbre_index);

    ESP_LOGI(TAG,
             "SF2 ready: timbre=%u path=%s bytes=%u load=%lldus preset=%s internal=%u->%u psram=%u->%u",
             timbre_index, path, (unsigned)file_size, (long long)(esp_timer_get_time() - start_us),
             tsf_get_presetname(font, preset_index), (unsigned)internal_before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)psram_before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

static void apply_expression_smoothing(uint32_t frames_to_render)
{
    if (!s_synth.font || s_synth.state != SAMPLER_SYNTH_STATE_READY)
        return;

    portENTER_CRITICAL(&s_synth.status_lock);
    const uint8_t latest_target = s_synth.expression_target;
    portEXIT_CRITICAL(&s_synth.status_lock);
    if (latest_target != s_synth.expression_ramp_target)
    {
        s_synth.expression_ramp_target = latest_target;
        s_synth.expression_frames_remaining =
            (s_synth.cfg.sample_rate * SYNTH_EXPRESSION_SMOOTH_MS) / 1000U;
    }

    if (s_synth.expression_frames_remaining > 0)
    {
        const uint32_t frames = frames_to_render < s_synth.expression_frames_remaining
                                    ? frames_to_render
                                    : s_synth.expression_frames_remaining;
        const float fraction = (float)frames / (float)s_synth.expression_frames_remaining;
        s_synth.expression_current +=
            ((float)s_synth.expression_ramp_target - s_synth.expression_current) * fraction;
        s_synth.expression_frames_remaining -= frames;
    }
    if (s_synth.expression_frames_remaining == 0)
        s_synth.expression_current = (float)s_synth.expression_ramp_target;

    for (uint8_t channel = 0; channel < SYNTH_CHANNEL_COUNT; ++channel)
    {
        /* CC11 只表达气息；换指增益由 TSF 后级逐采样混音器处理，不能再相乘造成阶梯。 */
        const uint8_t expression_cc11 = linear_expression_to_cc11(s_synth.expression_current);
        if (expression_cc11 != s_synth.expression_applied[channel])
        {
            tsf_channel_midi_control(s_synth.font, channel, 11, expression_cc11);
            s_synth.expression_applied[channel] = expression_cc11;
        }
        tsf_channel_set_mix_volume(s_synth.font, channel, s_synth.channel_gain[channel]);
    }
}

static void process_reverb(int16_t *samples, size_t frames)
{
    synth_reverb_t *reverb = &s_synth.reverb;
    if (!reverb->enabled || !reverb->buffer_l || !reverb->buffer_r)
        return;

    for (size_t frame = 0; frame < frames; ++frame)
    {
        float dry_l = (float)samples[frame * 2] / 32768.0f;
        float dry_r = (float)samples[frame * 2 + 1] / 32768.0f;
        float delayed_l = reverb->buffer_l[reverb->index_l];
        float delayed_r = reverb->buffer_r[reverb->index_r];

        reverb->last_l += reverb->damp * (delayed_l - reverb->last_l);
        reverb->last_r += reverb->damp * (delayed_r - reverb->last_r);
        reverb->buffer_l[reverb->index_l] = dry_l + reverb->last_l * reverb->feedback;
        reverb->buffer_r[reverb->index_r] = dry_r + reverb->last_r * reverb->feedback;
        reverb->index_l = (reverb->index_l + 1U) % reverb->delay_l;
        reverb->index_r = (reverb->index_r + 1U) % reverb->delay_r;

        float wet_l = delayed_l * 0.8f + delayed_r * 0.2f;
        float wet_r = delayed_r * 0.8f + delayed_l * 0.2f;
        samples[frame * 2] = clamp16((int32_t)((dry_l * (1.0f - reverb->wet) + wet_l * reverb->wet) * 32767.0f));
        samples[frame * 2 + 1] = clamp16((int32_t)((dry_r * (1.0f - reverb->wet) + wet_r * reverb->wet) * 32767.0f));
    }
}

static void reset_reverb(void)
{
    synth_reverb_t *reverb = &s_synth.reverb;
    reverb->last_l = reverb->last_r = 0.0f;
    reverb->index_l = reverb->index_r = 0;
    if (reverb->buffer_l)
        memset(reverb->buffer_l, 0, reverb->delay_l * sizeof(float));
    if (reverb->buffer_r)
        memset(reverb->buffer_r, 0, reverb->delay_r * sizeof(float));
}

/**
 * @brief 低开销统计最终 PCM 的削顶与相邻采样跳变，供低频诊断定位爆音。
 *
 * @param samples 已完成混响处理、即将提交给输出回调 的交错 PCM16 数据。
 * @param frames 数据帧数；每帧固定为左右两个采样。
 *
 * 这里只累加计数和峰值，不修改音频，也不在实时循环中打印日志。相邻跳变分别
 * 按左右声道跨块连续计算，因此既能看到块边界断点，也能看到块内数字削顶。
 */
static void analyze_output(const int16_t *samples, size_t frames, bool detailed)
{
    if (!samples || frames == 0)
        return;

    if (!detailed)
    {
        const size_t last = (frames - 1U) * SYNTH_OUTPUT_CHANNELS;
        for (uint8_t channel = 0; channel < SYNTH_OUTPUT_CHANNELS; ++channel)
            s_synth.previous_output[channel] = samples[last + channel];
        s_synth.previous_output_valid = true;
        return;
    }

    for (size_t frame = 0; frame < frames; ++frame)
    {
        for (uint8_t channel = 0; channel < SYNTH_OUTPUT_CHANNELS; ++channel)
        {
            const int16_t sample = samples[frame * SYNTH_OUTPUT_CHANNELS + channel];
            if (sample == INT16_MIN || sample == INT16_MAX)
                ++s_synth.clipped_samples;
            if (s_synth.previous_output_valid)
            {
                int32_t step = (int32_t)sample - (int32_t)s_synth.previous_output[channel];
                if (step < 0)
                    step = -step;
                if ((uint32_t)step > s_synth.max_sample_step)
                    s_synth.max_sample_step = (uint32_t)step;
            }
            s_synth.previous_output[channel] = sample;
        }
        s_synth.previous_output_valid = true;
    }
}

/* 高频 legato 需要在自然端点继续启动最新后继目标，故提前声明入口。 */
static void start_legato_transition(const synth_command_t *command);

static uint32_t legato_transition_frames(void)
{
    uint32_t frames =
        (s_synth.cfg.sample_rate * SYNTH_LEGATO_TRANSITION_MS) / 1000U;
    return frames > 1U ? frames : 2U;
}

static uint32_t legato_fade_out_frames(void)
{
    return legato_transition_frames() / 2U;
}

static uint32_t legato_fade_in_frames(void)
{
    return legato_transition_frames() - legato_fade_out_frames();
}

/**
 * @brief 在旧通道已经真实淡到零后切换演奏音符，并开始新音淡入。
 *
 * @param fade_frames 新音从零增益淡入到完整气息增益所用的帧数。
 * @return true 表示新 voice 创建成功；false 时保留旧 voice 并让调用方恢复旧音。
 *
 * 新旧音符的创建和回收发生在两次渲染之间，不在同一 PCM 帧叠加新旧音符。
 * SF2 叠层仍可能需要多个 voice；是否满足实时预算取决于音色、配置与硬件。
 */
static bool begin_legato_fade_in(uint32_t fade_frames)
{
    const uint8_t new_channel = s_synth.legato_new_channel;
    /* 两个通道此时都在真实零增益，必须硬回收旧 voice，不能留下 10 ms fast-release 计算。 */
    tsf_channel_kill_all(s_synth.font, new_channel);
    s_synth.channel_gain[new_channel] = 0.0f;
    if (!tsf_channel_note_on(s_synth.font, new_channel, s_synth.legato_new_note,
                             (float)s_synth.legato_velocity / 127.0f))
    {
        portENTER_CRITICAL(&s_synth.status_lock);
        s_synth.last_error = SAMPLER_SYNTH_ERROR_VOICE_ALLOC;
        portEXIT_CRITICAL(&s_synth.status_lock);
        return false;
    }

    tsf_channel_kill_all(s_synth.font, s_synth.legato_old_channel);
    s_synth.channel_gain[s_synth.legato_old_channel] = 0.0f;
    s_synth.active_channel = new_channel;
    s_synth.active_note = s_synth.legato_new_note;
    s_synth.legato_phase = SYNTH_LEGATO_FADE_IN;
    s_synth.legato_total_frames = fade_frames > 0U ? fade_frames : 1U;
    s_synth.legato_frames_remaining = s_synth.legato_total_frames;
    s_synth.pending_trace_seq = s_synth.legato_trace_seq;
    return true;
}

static void finish_legato_transition(void)
{
    if (!s_synth.legato_active)
        return;

    s_synth.channel_gain[s_synth.legato_old_channel] = 0.0f;
    s_synth.channel_gain[s_synth.legato_new_channel] = 1.0f;
    s_synth.active_channel = s_synth.legato_new_channel;
    s_synth.active_note = s_synth.legato_new_note;
    s_synth.legato_active = false;
    s_synth.legato_phase = SYNTH_LEGATO_FADE_OUT;
    s_synth.legato_frames_remaining = 0;
    s_synth.legato_total_frames = 0;
    s_synth.legato_old_note = -1;
    s_synth.legato_new_note = -1;
    s_synth.legato_velocity = 0;
    s_synth.legato_trace_seq = 0;
}

/**
 * @brief 为下一小块渲染准备当前淡出或淡入阶段的终点增益。
 *
 * TinySoundFont 会在这一小块内部从当前后级增益逐采样爬到这里设置的目标值。
 * 必须在渲染前准备目标，否则最后一个控制块结束时旧通道仍未真正到零，随后
 * note-off 或复用通道就会形成波形断点。
 *
 * @param frames_to_render 下一小块将渲染的帧数，不得超过当前过渡剩余帧数。
 */
static void prepare_legato_transition(uint32_t frames_to_render)
{
    if (!s_synth.legato_active || s_synth.legato_total_frames == 0)
        return;

    const uint32_t consumed = frames_to_render < s_synth.legato_frames_remaining
                                  ? frames_to_render
                                  : s_synth.legato_frames_remaining;
    const uint32_t remaining_after = s_synth.legato_frames_remaining - consumed;
    const float progress = 1.0f -
                           (float)remaining_after / (float)s_synth.legato_total_frames;
    if (s_synth.legato_phase == SYNTH_LEGATO_FADE_OUT)
    {
        s_synth.channel_gain[s_synth.legato_old_channel] =
            s_synth.legato_old_note >= 0 ? 1.0f - progress : 0.0f;
        s_synth.channel_gain[s_synth.legato_new_channel] = 0.0f;
    }
    else
    {
        s_synth.channel_gain[s_synth.legato_old_channel] = 0.0f;
        s_synth.channel_gain[s_synth.legato_new_channel] = progress;
    }
}

/**
 * @brief 提交已经完成渲染的过渡帧，并在真实零增益点串行切换 voice。
 *
 * @param rendered_frames 本轮已经渲染的帧数。
 */
static void advance_legato_transition(uint32_t rendered_frames)
{
    if (!s_synth.legato_active || s_synth.legato_total_frames == 0)
        return;

    if (rendered_frames >= s_synth.legato_frames_remaining)
    {
        if (s_synth.legato_phase == SYNTH_LEGATO_FADE_OUT)
        {
            if (!begin_legato_fade_in(legato_fade_in_frames()))
            {
                /* 分配失败时旧 voice 尚未回收；从真实零点平滑恢复旧音。 */
                s_synth.channel_gain[s_synth.legato_old_channel] =
                    s_synth.legato_old_note >= 0 ? 1.0f : 0.0f;
                s_synth.channel_gain[s_synth.legato_new_channel] = 0.0f;
                s_synth.legato_active = false;
                s_synth.legato_frames_remaining = 0;
                s_synth.legato_total_frames = 0;
            }
            return;
        }

        finish_legato_transition();
        if (s_synth.deferred_legato_valid)
        {
            const synth_command_t deferred = {
                .type = SYNTH_CMD_LEGATO,
                .note = s_synth.deferred_legato_note,
                .previous_note = s_synth.active_note,
                .velocity = s_synth.deferred_legato_velocity,
                .trace_seq = s_synth.deferred_legato_trace_seq,
                .audio_call_ts_us = s_synth.deferred_legato_call_ts_us,
            };
            s_synth.deferred_legato_valid = false;
            start_legato_transition(&deferred);
        }
        return;
    }
    s_synth.legato_frames_remaining -= rendered_frames;
}

/**
 * @brief 建立旧音淡出、零点换 voice、新音淡入的短 legato 过渡。
 *
 * @param command 原子 legato 命令；previous_note 仅在内部状态丢失时作为旧音提示。
 */
static void start_legato_transition(const synth_command_t *command)
{
    if (!s_synth.font || s_synth.state != SAMPLER_SYNTH_STATE_READY)
        return;

    if (command->note == s_synth.active_note && !s_synth.legato_active)
        return;

    /*
     * 高频换指可能在上一段 5 ms 过渡的最后一个控制块到来。这里只覆盖唯一的
     * 后继目标，待当前过渡自然到达端点后再启动，避免命令或 voice 累积。
     */
    if (s_synth.legato_active)
    {
        s_synth.deferred_legato_valid = true;
        s_synth.deferred_legato_note = command->note;
        s_synth.deferred_legato_velocity = command->velocity;
        s_synth.deferred_legato_trace_seq = command->trace_seq;
        s_synth.deferred_legato_call_ts_us = command->audio_call_ts_us;
        ++s_synth.deferred_legato_updates;
        return;
    }

    const uint8_t old_channel = s_synth.active_channel;
    const uint8_t new_channel = old_channel == SYNTH_PRIMARY_CHANNEL
                                    ? SYNTH_SECONDARY_CHANNEL
                                    : SYNTH_PRIMARY_CHANNEL;
    const int old_note = s_synth.active_note >= 0 ? s_synth.active_note : command->previous_note;

    s_synth.legato_old_channel = old_channel;
    s_synth.legato_new_channel = new_channel;
    s_synth.legato_old_note = old_note;
    s_synth.legato_new_note = command->note;
    s_synth.legato_velocity = command->velocity;
    s_synth.legato_trace_seq = command->trace_seq;
    s_synth.channel_gain[old_channel] = old_note >= 0 ? 1.0f : 0.0f;
    s_synth.channel_gain[new_channel] = 0.0f;
    s_synth.legato_active = true;
    s_synth.legato_phase = SYNTH_LEGATO_FADE_OUT;
    s_synth.output_analysis_frames_remaining =
        legato_transition_frames() + s_synth.cfg.chunk_frames;

    if (old_note >= 0)
    {
        s_synth.legato_total_frames = legato_fade_out_frames();
        s_synth.legato_frames_remaining = s_synth.legato_total_frames;
    }
    else if (!begin_legato_fade_in(legato_transition_frames()))
    {
        s_synth.legato_active = false;
        s_synth.legato_frames_remaining = 0;
        s_synth.legato_total_frames = 0;
    }
}

static void process_command(const synth_command_t *command)
{
    if (!command)
        return;

    switch (command->type)
    {
    case SYNTH_CMD_NOTE_ON:
        if (s_synth.font && s_synth.state == SAMPLER_SYNTH_STATE_READY)
        {
            s_synth.deferred_legato_valid = false;
            if (s_synth.legato_active)
                stop_all_channel_sounds(true);
            const uint8_t channel = s_synth.active_channel;
            const uint8_t inactive_channel = channel == SYNTH_PRIMARY_CHANNEL
                                                 ? SYNTH_SECONDARY_CHANNEL
                                                 : SYNTH_PRIMARY_CHANNEL;
            /* 完整 attack 前回收释放中的旧音，避免旧音与新音叠加消耗 voice 预算。 */
            for (uint8_t cleanup_channel = 0;
                 cleanup_channel < SYNTH_CHANNEL_COUNT; ++cleanup_channel)
                tsf_channel_kill_all(s_synth.font, cleanup_channel);
            s_synth.channel_gain[channel] = 1.0f;
            s_synth.channel_gain[inactive_channel] = 0.0f;
            if (tsf_channel_note_on(s_synth.font, channel, command->note,
                                    (float)command->velocity / 127.0f))
            {
                s_synth.active_note = command->note;
                s_synth.pending_trace_seq = command->trace_seq;
            }
            else
            {
                portENTER_CRITICAL(&s_synth.status_lock);
                s_synth.last_error = SAMPLER_SYNTH_ERROR_VOICE_ALLOC;
                portEXIT_CRITICAL(&s_synth.status_lock);
            }
        }
        break;
    case SYNTH_CMD_NOTE_OFF:
        if (s_synth.font && s_synth.state == SAMPLER_SYNTH_STATE_READY)
            stop_all_channel_sounds(false);
        break;
    case SYNTH_CMD_LEGATO:
        start_legato_transition(command);
        break;
    case SYNTH_CMD_LOAD_TIMBRE:
        load_soundfont_now(command->timbre_index, command->path);
        break;
    case SYNTH_CMD_SET_REVERB:
        s_synth.reverb.enabled = command->enabled && s_synth.reverb.buffer_l && s_synth.reverb.buffer_r;
        if (!s_synth.reverb.enabled)
            reset_reverb();
        break;
    case SYNTH_CMD_SHUTDOWN:
        destroy_font();
        s_synth.reverb.enabled = false;
        reset_reverb();
        s_synth.shutdown_frame_ok = false;
        s_synth.shutdown_frame_pending = true;
        set_status(SAMPLER_SYNTH_STATE_SHUTDOWN, SAMPLER_SYNTH_ERROR_NONE, -1);
        break;
    default:
        break;
    }
}

static void log_diagnostics(int64_t now_us)
{
    if (now_us - s_synth.last_diagnostic_us < SYNTH_DIAGNOSTIC_PERIOD_US)
        return;
    s_synth.last_diagnostic_us = now_us;
    ESP_LOGI(TAG, "diag state=%d voices=%d max_render=%uus steady=%uus legato=%uus max_write=%uus misses=%u window_misses=%u legato_misses=%u stalls=%u output_errors=%u clipped=%u max_step=%u legato_deferred=%u queue=%u internal=%u psram=%u",
             (int)s_synth.state, s_synth.font ? tsf_active_voice_count(s_synth.font) : 0,
             (unsigned)s_synth.max_render_us, (unsigned)s_synth.max_render_steady_us,
             (unsigned)s_synth.max_render_legato_us, (unsigned)s_synth.max_output_write_us,
             (unsigned)s_synth.deadline_misses, (unsigned)s_synth.window_deadline_misses,
             (unsigned)s_synth.window_legato_misses, (unsigned)s_synth.output_stalls,
             (unsigned)s_synth.output_errors, (unsigned)s_synth.clipped_samples,
             (unsigned)s_synth.max_sample_step, (unsigned)s_synth.deferred_legato_updates,
             s_synth.command_queue ? (unsigned)uxQueueMessagesWaiting(s_synth.command_queue) : 0U,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    s_synth.max_render_us = 0;
    s_synth.max_render_steady_us = 0;
    s_synth.max_render_legato_us = 0;
    s_synth.max_output_write_us = 0;
    s_synth.window_deadline_misses = 0;
    s_synth.window_legato_misses = 0;
    s_synth.max_sample_step = 0;
}

/**
 * @brief 独占音色、命令处理和 PCM 渲染，通过应用回调提交输出。
 * @param argument 未使用；所有缓冲在 init 时准备。
 * 每块最多处理队列容量个命令，避免生产者连续填队列导致渲染饥饿。
 * 输出回调负责音频节拍且必须有限时返回；退出信号发出后不再访问内部资源。
 */
static void render_task(void *argument)
{
    (void)argument;
    const size_t sample_count = (size_t)s_synth.cfg.chunk_frames * SYNTH_OUTPUT_CHANNELS;
    const size_t output_bytes = sample_count * sizeof(int16_t);
    int16_t *output = s_synth.output;

    s_synth.last_diagnostic_us = esp_timer_get_time();
    while (s_synth.running)
    {
        synth_command_t command;
        for (uint16_t handled = 0; handled < s_synth.cfg.command_queue_length; ++handled)
        {
            if (xQueueReceive(s_synth.command_queue, &command, 0) != pdTRUE)
                break;
            process_command(&command);
            /* 先提交零块并确认 shutdown，不能在同一块又被后续加载命令覆盖。 */
            if (command.type == SYNTH_CMD_SHUTDOWN)
                break;
        }

        const bool rendered_legato = s_synth.legato_active;
        int64_t render_start = esp_timer_get_time();
        if (s_synth.font && s_synth.state == SAMPLER_SYNTH_STATE_READY)
        {
            uint32_t rendered_frames = 0;
            while (rendered_frames < s_synth.cfg.chunk_frames)
            {
                const uint32_t remaining = s_synth.cfg.chunk_frames - rendered_frames;
                uint32_t control_frames = remaining < SYNTH_CONTROL_BLOCK_FRAMES
                                              ? remaining
                                              : SYNTH_CONTROL_BLOCK_FRAMES;
                /* 最后一小块精确截到过渡端点，保证随后复用的通道已经逐采样淡到零。 */
                if (s_synth.legato_active &&
                    s_synth.legato_frames_remaining < control_frames)
                    control_frames = s_synth.legato_frames_remaining;
                prepare_legato_transition(control_frames);
                apply_expression_smoothing(control_frames);
                tsf_render_short(s_synth.font,
                                 output + (size_t)rendered_frames * SYNTH_OUTPUT_CHANNELS,
                                 (int)control_frames, 0);
                advance_legato_transition(control_frames);
                rendered_frames += control_frames;
            }
            process_reverb(output, s_synth.cfg.chunk_frames);
            if (s_synth.pending_trace_seq != 0 && s_trace_callback)
            {
                s_trace_callback(s_synth.pending_trace_seq,
                                 tsf_active_voice_count(s_synth.font), esp_timer_get_time());
                s_synth.pending_trace_seq = 0;
            }
        }
        else
        {
            memset(output, 0, output_bytes);
        }
        const bool analyze_detailed = s_synth.output_analysis_frames_remaining > 0;
        analyze_output(output, s_synth.cfg.chunk_frames, analyze_detailed);
        if (analyze_detailed)
        {
            if (s_synth.output_analysis_frames_remaining > s_synth.cfg.chunk_frames)
                s_synth.output_analysis_frames_remaining -= s_synth.cfg.chunk_frames;
            else
                s_synth.output_analysis_frames_remaining = 0;
        }
        uint32_t render_us = (uint32_t)(esp_timer_get_time() - render_start);
        if (render_us > s_synth.max_render_us)
            s_synth.max_render_us = render_us;
        uint32_t *mode_max = rendered_legato
                                 ? &s_synth.max_render_legato_us
                                 : &s_synth.max_render_steady_us;
        if (render_us > *mode_max)
            *mode_max = render_us;
        uint32_t deadline_us = (uint32_t)(((uint64_t)s_synth.cfg.chunk_frames * 1000000ULL) / s_synth.cfg.sample_rate);
        if (render_us >= deadline_us)
        {
            ++s_synth.deadline_misses;
            ++s_synth.window_deadline_misses;
            if (rendered_legato)
                ++s_synth.window_legato_misses;
        }

        const int64_t output_write_start = esp_timer_get_time();
        const bool output_write_ok = s_synth.cfg.output_write(
            output, s_synth.cfg.chunk_frames, s_synth.cfg.output_ctx);
        const uint32_t output_write_us =
            (uint32_t)(esp_timer_get_time() - output_write_start);
        if (output_write_us > s_synth.max_output_write_us)
            s_synth.max_output_write_us = output_write_us;
        if (output_write_us > deadline_us * 2U)
            ++s_synth.output_stalls;
        if (!output_write_ok)
        {
            ++s_synth.output_errors;
            portENTER_CRITICAL(&s_synth.status_lock);
            s_synth.last_error = SAMPLER_SYNTH_ERROR_OUTPUT;
            portEXIT_CRITICAL(&s_synth.status_lock);
            /* 输出故障可能立即返回，至少让出一个 tick，避免高优先级空转。 */
            vTaskDelay(1);
        }

        /*
         * shutdown 命令把合成状态切到 SHUTDOWN 后，本轮渲染必然生成全零 PCM。
         * 必须等这帧实际交给输出回调 后再唤醒关机调用方，避免 调用方过早切断功放。
         */
        if (s_synth.shutdown_frame_pending)
        {
            s_synth.shutdown_frame_ok = output_write_ok;
            s_synth.shutdown_frame_pending = false;
            if (s_synth.shutdown_sem)
                xSemaphoreGive(s_synth.shutdown_sem);
        }
        if (s_synth.cfg.diagnostics)
            log_diagnostics(esp_timer_get_time());
    }

    destroy_font();
    if (s_synth.stopped_sem)
        xSemaphoreGive(s_synth.stopped_sem);
    vTaskDelete(NULL);
}

/**
 * @brief 在任务退出或创建失败后释放资源，不接触应用硬件。
 * 只能由生命周期调用方执行；渲染任务仍运行时严禁调用。
 */
static void release_resources(void)
{
    destroy_font();
    if (s_synth.command_queue)
        vQueueDelete(s_synth.command_queue);
    if (s_synth.stopped_sem)
        vSemaphoreDelete(s_synth.stopped_sem);
    if (s_synth.shutdown_sem)
        vSemaphoreDelete(s_synth.shutdown_sem);
    s_synth.command_queue = NULL;
    s_synth.stopped_sem = s_synth.shutdown_sem = NULL;
    s_synth.task = NULL;
    heap_caps_free(s_synth.output);
    s_synth.output = NULL;
    free(s_synth.reverb.buffer_l);
    free(s_synth.reverb.buffer_r);
    memset(&s_synth.reverb, 0, sizeof(s_synth.reverb));
    s_synth.shutdown_frame_pending = false;
    s_synth.shutdown_frame_ok = false;
}

/**
 * @brief 复制并验证配置，在任务创建前准备资源，失败统一回收。
 * @param cfg 调用方配置，必须提供有节拍、有界等待的 PCM 输出回调。
 * @return true 表示任务创建成功；失败不会留下队列、缓冲或半初始化任务。
 * 不初始化 codec/SD/I2S，不持有应用硬件所有权。生命周期由应用串行化。
 */
bool sampler_synth_init(const sampler_synth_cfg_t *cfg)
{
    if (s_synth.command_queue || s_synth.task)
        return false;
    if (!cfg || !cfg->output_write ||
        (cfg->sample_rate != 0 && (cfg->sample_rate < 8000 || cfg->sample_rate > 192000)) ||
        cfg->bank > 16383 || cfg->preset > 127 || cfg->zone_velocity > 127 ||
        (cfg->validate_zones && (cfg->note_min > cfg->note_max || cfg->note_max > 127)) ||
        cfg->task_priority >= configMAX_PRIORITIES ||
        (cfg->pin_task_to_core && cfg->task_core >= portNUM_PROCESSORS))
    {
        set_status(SAMPLER_SYNTH_STATE_ERROR, SAMPLER_SYNTH_ERROR_INVALID_ARGUMENT, -1);
        return false;
    }
    s_synth.cfg = *cfg;
    if (s_synth.cfg.sample_rate == 0)
        s_synth.cfg.sample_rate = SYNTH_DEFAULT_RATE;
    if (s_synth.cfg.chunk_frames == 0)
        s_synth.cfg.chunk_frames = SYNTH_DEFAULT_CHUNK_FRAMES;
    if (s_synth.cfg.max_voices == 0)
        s_synth.cfg.max_voices = SYNTH_DEFAULT_MAX_VOICES;
    if (s_synth.cfg.max_sf2_bytes == 0)
        s_synth.cfg.max_sf2_bytes = SAMPLER_SYNTH_SF2_MAX_BYTES;
    if (s_synth.cfg.zone_velocity == 0)
        s_synth.cfg.zone_velocity = 100;
    if (s_synth.cfg.command_queue_length == 0)
        s_synth.cfg.command_queue_length = SYNTH_COMMAND_QUEUE_LENGTH;
    if (s_synth.cfg.task_stack_bytes == 0)
        s_synth.cfg.task_stack_bytes = SYNTH_TASK_STACK_SIZE;
    if (s_synth.cfg.task_priority == 0)
        s_synth.cfg.task_priority = SYNTH_TASK_PRIORITY;
    if (s_synth.cfg.task_stack_bytes < 4096)
    {
        set_status(SAMPLER_SYNTH_STATE_ERROR, SAMPLER_SYNTH_ERROR_INVALID_ARGUMENT, -1);
        return false;
    }

    s_synth.command_queue = xQueueCreate(s_synth.cfg.command_queue_length, sizeof(synth_command_t));
    s_synth.stopped_sem = xSemaphoreCreateBinary();
    s_synth.shutdown_sem = xSemaphoreCreateBinary();
    s_synth.output = heap_caps_calloc((size_t)s_synth.cfg.chunk_frames * SYNTH_OUTPUT_CHANNELS,
                                     sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_synth.command_queue || !s_synth.stopped_sem || !s_synth.shutdown_sem || !s_synth.output)
        goto memory_failure;

    s_synth.reverb.wet = 0.25f;
    s_synth.reverb.feedback = 0.60f;
    s_synth.reverb.damp = 0.50f;
    s_synth.reverb.delay_l = (size_t)((s_synth.cfg.sample_rate * 150U) / 1000U);
    s_synth.reverb.delay_r = (size_t)((s_synth.cfg.sample_rate * 190U) / 1000U);
    s_synth.reverb.buffer_l = calloc(s_synth.reverb.delay_l, sizeof(float));
    s_synth.reverb.buffer_r = calloc(s_synth.reverb.delay_r, sizeof(float));
    if (!s_synth.reverb.buffer_l || !s_synth.reverb.buffer_r)
    {
        ESP_LOGW(TAG, "reverb buffers unavailable; reverb disabled");
        free(s_synth.reverb.buffer_l);
        free(s_synth.reverb.buffer_r);
        s_synth.reverb.buffer_l = s_synth.reverb.buffer_r = NULL;
    }

    s_synth.expression_target = 0;
    s_synth.expression_current = 0.0f;
    s_synth.expression_ramp_target = 0;
    s_synth.expression_frames_remaining = 0;
    reset_performance_state();
    s_synth.pending_trace_seq = 0;
    s_synth.max_render_us = 0;
    s_synth.max_render_steady_us = 0;
    s_synth.max_render_legato_us = 0;
    s_synth.max_output_write_us = 0;
    s_synth.deadline_misses = 0;
    s_synth.window_deadline_misses = 0;
    s_synth.window_legato_misses = 0;
    s_synth.output_stalls = 0;
    s_synth.output_errors = 0;
    s_synth.clipped_samples = 0;
    s_synth.max_sample_step = 0;
    s_synth.previous_output_valid = false;
    s_synth.output_analysis_frames_remaining = 0;
    s_synth.deferred_legato_updates = 0;
    s_synth.shutdown_frame_pending = false;
    s_synth.shutdown_frame_ok = false;
    set_status(SAMPLER_SYNTH_STATE_LOADING, SAMPLER_SYNTH_ERROR_NONE, -1);
    s_synth.running = true;
    const BaseType_t core = s_synth.cfg.pin_task_to_core ? s_synth.cfg.task_core : tskNO_AFFINITY;
    if (xTaskCreatePinnedToCore(render_task, "sf2_render", s_synth.cfg.task_stack_bytes,
                               NULL, s_synth.cfg.task_priority, &s_synth.task, core) != pdPASS)
    {
        s_synth.running = false;
        goto memory_failure;
    }
    ESP_LOGI(TAG, "render started: %u Hz, %u frames, voices=%u",
             (unsigned)s_synth.cfg.sample_rate, (unsigned)s_synth.cfg.chunk_frames,
             (unsigned)s_synth.cfg.max_voices);
    return true;

memory_failure:
    release_resources();
    set_status(SAMPLER_SYNTH_STATE_ERROR, SAMPLER_SYNTH_ERROR_MEMORY, -1);
    return false;
}

/**
 * @brief 停止任务并在退出确认后回收资源，避免输出回调仍使用缓冲时提前释放。
 * 调用方须先停止所有生产者；此函数不能与其他 API 并发，不得从回调中调用。
 * 不强制删除任务，等待时间取决于应用输出回调的有限超时和正在进行的文件读取。
 */
void sampler_synth_deinit(void)
{
    if (s_synth.task && xTaskGetCurrentTaskHandle() == s_synth.task)
    {
        ESP_LOGE(TAG, "deinit cannot run inside a render callback");
        return;
    }
    s_synth.running = false;
    if (s_synth.task && s_synth.stopped_sem)
        xSemaphoreTake(s_synth.stopped_sem, portMAX_DELAY);
    release_resources();
    set_status(SAMPLER_SYNTH_STATE_SHUTDOWN, SAMPLER_SYNTH_ERROR_NONE, -1);
}

bool sampler_synth_load_soundfont(uint8_t timbre_index, const char *path)
{
    if (!path || !path[0] || strlen(path) >= sizeof(((synth_command_t *)0)->path))
        return false;
    synth_command_t command = {.type = SYNTH_CMD_LOAD_TIMBRE, .timbre_index = timbre_index};
    memcpy(command.path, path, strlen(path) + 1U);
    return enqueue_command(&command);
}

bool sampler_synth_note_on(int midi_note, uint8_t velocity, uint32_t trace_seq, int64_t audio_call_ts_us)
{
    if (midi_note < 0 || midi_note > 127 || velocity == 0 || velocity > 127)
        return false;
    synth_command_t command = {
        .type = SYNTH_CMD_NOTE_ON,
        .note = midi_note,
        .velocity = velocity,
        .trace_seq = trace_seq,
        .audio_call_ts_us = audio_call_ts_us,
    };
    return enqueue_command(&command);
}

bool sampler_synth_note_off(int midi_note, uint32_t trace_seq, int64_t audio_call_ts_us)
{
    if (midi_note < 0 || midi_note > 127)
        return false;
    synth_command_t command = {
        .type = SYNTH_CMD_NOTE_OFF,
        .note = midi_note,
        .trace_seq = trace_seq,
        .audio_call_ts_us = audio_call_ts_us,
    };
    return enqueue_command(&command);
}

bool sampler_synth_legato_transition(int previous_note, int target_note, uint8_t velocity,
                                     uint32_t trace_seq, int64_t audio_call_ts_us)
{
    if (previous_note < -1 || previous_note > 127 || target_note < 0 || target_note > 127 ||
        velocity == 0 || velocity > 127)
        return false;
    synth_command_t command = {
        .type = SYNTH_CMD_LEGATO,
        .note = target_note,
        .previous_note = previous_note,
        .velocity = velocity,
        .trace_seq = trace_seq,
        .audio_call_ts_us = audio_call_ts_us,
    };
    return enqueue_command(&command);
}

void sampler_synth_set_expression(uint8_t expression)
{
    portENTER_CRITICAL(&s_synth.status_lock);
    s_synth.expression_target = expression > 127 ? 127 : expression;
    portEXIT_CRITICAL(&s_synth.status_lock);
}

bool sampler_synth_set_reverb(bool enable)
{
    if (enable && (!s_synth.reverb.buffer_l || !s_synth.reverb.buffer_r))
        return false;
    synth_command_t command = {.type = SYNTH_CMD_SET_REVERB, .enabled = enable};
    return enqueue_command(&command);
}

bool sampler_synth_shutdown(void)
{
    if (!s_synth.shutdown_sem || (s_synth.task && xTaskGetCurrentTaskHandle() == s_synth.task))
        return false;

    /* 清除可能残留的完成信号，确保本次等待只对应本次 shutdown 命令。 */
    while (xSemaphoreTake(s_synth.shutdown_sem, 0) == pdTRUE)
    {
    }

    synth_command_t command = {.type = SYNTH_CMD_SHUTDOWN};
    if (!enqueue_command(&command))
        return false;

    /*
     * 等待渲染任务完成 shutdown，并把至少一帧全零 PCM 提交给输出回调。
     * 使用有限超时避免输出端异常时永久卡住最终关机流程。
     */
    if (xSemaphoreTake(s_synth.shutdown_sem, pdMS_TO_TICKS(SYNTH_SHUTDOWN_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(TAG, "timed out waiting for shutdown silence frame");
        return false;
    }
    return s_synth.shutdown_frame_ok;
}

void sampler_synth_set_trace_callback(sampler_synth_trace_cb_t callback)
{
    if (!s_synth.command_queue && !s_synth.task)
        s_trace_callback = callback;
}

sampler_synth_state_t sampler_synth_get_state(void)
{
    portENTER_CRITICAL(&s_synth.status_lock);
    sampler_synth_state_t state = s_synth.state;
    portEXIT_CRITICAL(&s_synth.status_lock);
    return state;
}

sampler_synth_error_t sampler_synth_get_last_error(void)
{
    portENTER_CRITICAL(&s_synth.status_lock);
    sampler_synth_error_t error = s_synth.last_error;
    portEXIT_CRITICAL(&s_synth.status_lock);
    return error;
}

int sampler_synth_get_loaded_timbre(void)
{
    portENTER_CRITICAL(&s_synth.status_lock);
    int timbre = s_synth.loaded_timbre;
    portEXIT_CRITICAL(&s_synth.status_lock);
    return timbre;
}

const char *sampler_synth_error_name(sampler_synth_error_t error)
{
    switch (error)
    {
    case SAMPLER_SYNTH_ERROR_NONE: return "none";
    case SAMPLER_SYNTH_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case SAMPLER_SYNTH_ERROR_FILE_OPEN: return "file open";
    case SAMPLER_SYNTH_ERROR_FILE_SIZE: return "file size";
    case SAMPLER_SYNTH_ERROR_PSRAM: return "PSRAM";
    case SAMPLER_SYNTH_ERROR_PARSE: return "SF2 parse";
    case SAMPLER_SYNTH_ERROR_PRESET: return "bank / preset";
    case SAMPLER_SYNTH_ERROR_ZONE: return "zone contract";
    case SAMPLER_SYNTH_ERROR_VOICE_ALLOC: return "voice allocation";
    case SAMPLER_SYNTH_ERROR_QUEUE_FULL: return "command queue full";
    case SAMPLER_SYNTH_ERROR_OUTPUT: return "PCM output";
    case SAMPLER_SYNTH_ERROR_MEMORY: return "resource allocation";
    default: return "unknown";
    }
}

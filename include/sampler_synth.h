// SPDX-License-Identifier: MIT
// Copyright (c) 2026 xc-soundfont-synth contributors
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 历史宏名，表示默认文件预算；配置可覆盖，不是硬上限。 */
#define SAMPLER_SYNTH_SF2_MAX_BYTES (512U * 1024U)
#define SAMPLER_SYNTH_OUTPUT_CHANNELS 2U
#define SAMPLER_SYNTH_MIDI_NOTE_MIN 0
#define SAMPLER_SYNTH_MIDI_NOTE_MAX 127

typedef enum
{
    SAMPLER_SYNTH_STATE_LOADING = 0,
    SAMPLER_SYNTH_STATE_READY,
    SAMPLER_SYNTH_STATE_ERROR,
    SAMPLER_SYNTH_STATE_SHUTDOWN,
} sampler_synth_state_t;

typedef enum
{
    SAMPLER_SYNTH_ERROR_NONE = 0,
    SAMPLER_SYNTH_ERROR_INVALID_ARGUMENT,
    SAMPLER_SYNTH_ERROR_FILE_OPEN,
    SAMPLER_SYNTH_ERROR_FILE_SIZE,
    SAMPLER_SYNTH_ERROR_PSRAM,
    SAMPLER_SYNTH_ERROR_PARSE,
    SAMPLER_SYNTH_ERROR_PRESET,
    SAMPLER_SYNTH_ERROR_ZONE,
    SAMPLER_SYNTH_ERROR_VOICE_ALLOC,
    SAMPLER_SYNTH_ERROR_QUEUE_FULL,
    SAMPLER_SYNTH_ERROR_CODEC, /* 保留旧数值，新代码使用 OUTPUT。 */
    SAMPLER_SYNTH_ERROR_OUTPUT = SAMPLER_SYNTH_ERROR_CODEC,
    SAMPLER_SYNTH_ERROR_MEMORY,
} sampler_synth_error_t;

/**
 * @brief 在渲染任务中提交完整的 PCM16 交错立体声块。
 * @param samples 临时只读缓冲，L,R,L,R 排列，返回后不得保留指针。
 * @param frames 每声道帧数；总字节数为 frames * 2 * sizeof(int16_t)。
 * @param user_ctx 调用方持有的上下文，必须存活到 deinit 返回。
 * @return true 表示整块已写入或复制；部分写入、超时必须返回 false。
 * 回调负责按音频时钟提供背压，例如有界超时的 i2s_channel_write。
 * 不得无限等待、操作 UI/存储，或调用 shutdown/deinit。异步 DMA 不能直接
 * 引用 samples。组件不初始化、停止或释放 I2S、codec、功放。
 */
typedef bool (*sampler_synth_output_cb_t)(const int16_t *samples, size_t frames, void *user_ctx);

typedef struct
{
    uint32_t sample_rate;
    uint16_t chunk_frames;
    uint8_t max_voices;
    size_t max_sf2_bytes;
    sampler_synth_output_cb_t output_write; /* 必填，运行中不可更换。 */
    void *output_ctx;
    uint16_t bank;              /* 默认 0，范围 0..16383。 */
    uint8_t preset;             /* 默认 0，范围 0..127。 */
    bool validate_zones;       /* 默认关闭，加载时检查下列音域/力度。 */
    uint8_t note_min;
    uint8_t note_max;
    uint8_t zone_velocity;     /* 0 使用 100。 */
    uint8_t max_zones_per_note; /* 0 不限制层数；开启检查时每音至少一层。 */
    size_t min_free_psram_bytes; /* 0 不强制 PSRAM；非零为加载前空闲门槛。 */
    uint16_t command_queue_length; /* 0 使用 16。 */
    uint32_t task_stack_bytes; /* ESP-IDF 按字节计，0 使用 6144。 */
    uint8_t task_priority;     /* 0 使用 8，必须小于 configMAX_PRIORITIES。 */
    bool pin_task_to_core;     /* 默认不绑核，兼容单核。 */
    uint8_t task_core;         /* 仅 pin_task_to_core=true 时有效。 */
    bool diagnostics;         /* 默认关闭；开启后每约 10 秒输出统计。 */
} sampler_synth_cfg_t;

/**
 * @brief 渲染完成、输出提交前的追踪回调，不得阻塞，只允许在未初始化时设置。
 * trace_seq 是非零的调用方序号；voice_id 为历史名称，实际是活动 voice 数量；
 * render_ts_us 是 esp_timer 时间，不代表 DAC 或扬声器出声时间。
 */
typedef void (*sampler_synth_trace_cb_t)(uint32_t trace_seq, int voice_id, int64_t render_ts_us);

/**
 * @brief 启动单实例合成器，不访问板级硬件。cfg 的 output_write 必填。
 * @param cfg 配置会复制；sample_rate/chunk_frames/max_voices/max_sf2_bytes
 * 为 0 时分别使用 44100/128/8/512 KiB；非零大小预算不再限制为 512 KiB。
 * @return true 表示资源分配和任务创建成功，重复初始化或失败返回 false。
 * 初始 LOADING 表示等待首次加载；初始 expression=0，混响关闭。
 * 生命周期 API 必须串行调用；销毁时必须暂停所有生产者，不能与其他 API 并发。
 */
bool sampler_synth_init(const sampler_synth_cfg_t *cfg);
/**
 * @brief 等待任务退出并释放内部资源，不关闭输出设备或文件系统。
 * 调用前停止生产者，建议先 shutdown；输出回调必须有界返回。
 * 本函数会等待，不得在渲染回调、ISR 或音频实时任务中调用。
 */
void sampler_synth_deinit(void);

/**
 * @brief 异步加载已挂载 VFS 路径；timbre_index 是调用方标签，不是 SF2 preset。
 * @param path 非空路径，最多 127 字节；命令复制路径，不持有原字符串。
 * @return true 仅表示入队；需检查 state/error 确认实际加载结果。
 * 加载先释放旧音色，失败静音；读取/解析会暂停音频提交，切换应在非演奏期进行。
 */
bool sampler_synth_load_soundfont(uint8_t timbre_index, const char *path);
/**
 * @brief 异步起音，会结束先前演奏音；midi_note=0..127，velocity=1..127。
 * trace_seq=0 禁用追踪，audio_call_ts_us 为可选时间戳；true 只表示入队。
 * 应先确认 READY 并设置非零 expression；voice 预算不表示多音符复音。
 */
bool sampler_synth_note_on(int midi_note, uint8_t velocity, uint32_t trace_seq, int64_t audio_call_ts_us);
/**
 * @brief 异步停止当前演奏音并取消连音；true 仅表示入队，满队列须由调用方重试。
 * midi_note=0..127，仅保留旧接口形式；单音实现会关闭全部演奏通道，不按音符筛选。
 * trace_seq 为追踪序号，audio_call_ts_us 为可选调用时间戳。
 */
bool sampler_synth_note_off(int midi_note, uint32_t trace_seq, int64_t audio_call_ts_us);

/**
 * @brief 在连续气息中把当前音符平滑过渡到目标音符。
 *
 * 命令以单个队列项提交；排队失败时调用者可以继续保留旧音。previous_note 为 -1 时
 * 表示气息仍连续但当前没有有效音符，新音会从零通道增益柔和淡入。
 *
 * @param previous_note 当前音符，允许为 -1。
 * @param target_note 目标 MIDI 音符。
 * @param velocity SF2 note-on 力度。
 * @param trace_seq 延迟追踪序号。
 * @param audio_call_ts_us 音频服务调用时间。
 * @return true 表示命令已进入合成队列。
 */
bool sampler_synth_legato_transition(int previous_note, int target_note, uint8_t velocity,
                                     uint32_t trace_seq, int64_t audio_call_ts_us);
/** @brief 更新最新线性响度 0..127（超限截断），约 5 ms 平滑，不经过命令队列。 */
void sampler_synth_set_expression(uint8_t expression);
/** @brief 异步开关轻量混响，关闭时清除尾音；true 只表示入队。 */
bool sampler_synth_set_reverb(bool enable);

/**
 * @brief 释放音色并等待一块全零 PCM 被输出回调接受；不代表 DMA 已播放完。
 *
 * @return true 表示零块提交成功；排队失败、输出失败或 1 秒超时返回 false。
 * 调用前停止生产者，不得与其他控制命令并发调用；调用方负责排空及硬件静音。
 * 本操作不会退出任务，之后仍可重新加载音色。
 */
bool sampler_synth_shutdown(void);
void sampler_synth_set_trace_callback(sampler_synth_trace_cb_t cb);

sampler_synth_state_t sampler_synth_get_state(void);
sampler_synth_error_t sampler_synth_get_last_error(void);
int sampler_synth_get_loaded_timbre(void);
const char *sampler_synth_error_name(sampler_synth_error_t error);

#ifdef __cplusplus
}
#endif

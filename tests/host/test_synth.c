// SPDX-License-Identifier: MIT
#include "platform.h"
/* 直接编译生产实现和真实 TinySoundFont；仅平台 API 使用替身。 */
#include "../../sampler_synth.c"

static unsigned blocks, nonzero_blocks, transitions, failed_blocks;
static bool exercise_controls, force_output_failure, inspect_shutdown;
static unsigned stop_after = 30;

/**
 * @brief 确定性接收真实渲染 PCM，并在块边界投递控制命令。
 * 主机测试直接终止任务循环，不能把这种调度替身作为产品的输出后端。
 */
static bool capture_pcm(const int16_t *pcm, size_t frames, void *ctx)
{
    assert(ctx == &blocks && pcm && frames == 128);
    ++blocks;
    bool nonzero = false;
    for (size_t i = 0; i < frames * 2; ++i) nonzero |= pcm[i] != 0;
    if (nonzero) ++nonzero_blocks;
    if (inspect_shutdown) assert(!nonzero);
    if (s_synth.legato_active) ++transitions;
    if (exercise_controls)
    {
        assert(!sampler_synth_shutdown()); /* 防止渲染回调自等待。 */
        if (blocks == 3) assert(sampler_synth_legato_transition(60, 64, 100, 0, 0));
        if (blocks == 4) assert(sampler_synth_legato_transition(64, 67, 100, 0, 0));
        if (blocks == 16) assert(sampler_synth_note_off(67, 0, 0));
    }
    if (blocks >= stop_after) s_synth.running = false;
    if (force_output_failure) { ++failed_blocks; return false; }
    return true;
}

static sampler_synth_cfg_t config(void)
{
    sampler_synth_cfg_t cfg = {0};
    cfg.output_write = capture_pcm;
    cfg.output_ctx = &blocks;
    return cfg;
}
static void reset_capture(void)
{
    blocks = nonzero_blocks = transitions = failed_blocks = 0;
    exercise_controls = force_output_failure = inspect_shutdown = false;
    stop_after = 30;
}
static void finish(void)
{
    sampler_synth_deinit();
    assert(host_resources == 0 && !s_synth.reverb.buffer_l && !s_synth.reverb.buffer_r);
    assert(sampler_synth_get_state() == SAMPLER_SYNTH_STATE_SHUTDOWN);
}
static void assert_load_error(const char *path, sampler_synth_cfg_t cfg, sampler_synth_error_t expected)
{
    reset_capture();
    assert(sampler_synth_init(&cfg));
    assert(sampler_synth_load_soundfont(0, path));
    host_run_task();
    assert(nonzero_blocks == 0);
    assert(sampler_synth_get_state() == SAMPLER_SYNTH_STATE_ERROR);
    assert(sampler_synth_get_last_error() == expected);
    finish();
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    const char *sf2 = argv[1], *bad = argv[2];
    sampler_synth_cfg_t cfg = config();
    assert(!sampler_synth_init(NULL));
    sampler_synth_cfg_t invalid = {0};
    assert(!sampler_synth_init(&invalid));
    invalid = cfg; invalid.pin_task_to_core = true; invalid.task_core = 1;
    assert(!sampler_synth_init(&invalid)); /* 主机按单核配置构建。 */
    invalid = cfg; invalid.sample_rate = 1; assert(!sampler_synth_init(&invalid));
    invalid = cfg; invalid.task_priority = configMAX_PRIORITIES; assert(!sampler_synth_init(&invalid));
    invalid = cfg; invalid.validate_zones = true; invalid.note_min = 70; invalid.note_max = 60;
    assert(!sampler_synth_init(&invalid));
    assert(host_resources == 0);
    puts("PASS argument validation and single-core affinity");

    host_fail_queue = true; assert(!sampler_synth_init(&cfg)); host_fail_queue = false;
    assert(host_resources == 0);
    for (int i = 1; i <= 2; ++i)
    {
        host_fail_sem = i; assert(!sampler_synth_init(&cfg)); assert(host_resources == 0);
    }
    host_fail_output_alloc = true; assert(!sampler_synth_init(&cfg)); host_fail_output_alloc = false;
    host_fail_task = true; assert(!sampler_synth_init(&cfg)); host_fail_task = false;
    assert(host_resources == 0);
    puts("PASS queue/semaphore/buffer/task allocation failure cleanup");

    reset_capture();
    assert(sampler_synth_init(&cfg));
    assert(!sampler_synth_init(&cfg));
    assert(!sampler_synth_note_on(60, 0, 0, 0));
    assert(!sampler_synth_note_on(128, 100, 0, 0));
    assert(!sampler_synth_load_soundfont(0, ""));
    assert(sampler_synth_load_soundfont(7, sf2));
    sampler_synth_set_expression(100);
    assert(sampler_synth_note_on(60, 100, 0, 0));
    assert(sampler_synth_set_reverb(true));
    exercise_controls = true;
    host_run_task();
    assert(nonzero_blocks > 0 && transitions > 0 && blocks == stop_after);
    assert(sampler_synth_get_loaded_timbre() == 7);
    finish();
    puts("PASS real SF2 parsing, PCM rendering, expression, legato, reverb and note off");

    cfg = config(); cfg.bank = 2; cfg.preset = 3;
    reset_capture(); assert(sampler_synth_init(&cfg));
    assert(sampler_synth_load_soundfont(1, sf2));
    sampler_synth_set_expression(100); assert(sampler_synth_note_on(62, 100, 0, 0));
    host_run_task(); assert(nonzero_blocks > 0); finish();
    cfg = config(); cfg.bank = 1;
    assert_load_error(sf2, cfg, SAMPLER_SYNTH_ERROR_PRESET);
    cfg = config(); cfg.validate_zones = true; cfg.note_min = 60; cfg.note_max = 68;
    assert_load_error(sf2, cfg, SAMPLER_SYNTH_ERROR_ZONE);
    cfg = config(); cfg.max_sf2_bytes = 1;
    assert_load_error(sf2, cfg, SAMPLER_SYNTH_ERROR_FILE_SIZE);
    cfg = config(); cfg.min_free_psram_bytes = 1;
    assert_load_error(sf2, cfg, SAMPLER_SYNTH_ERROR_PSRAM);
    cfg = config();
    assert_load_error(bad, cfg, SAMPLER_SYNTH_ERROR_PARSE);
    assert_load_error("__xc_missing_file__.sf2", cfg, SAMPLER_SYNTH_ERROR_FILE_OPEN);
    puts("PASS bank/preset selection and silent rejection of invalid fonts/budgets/zones");

    reset_capture(); cfg = config(); cfg.command_queue_length = 1;
    assert(sampler_synth_init(&cfg));
    assert(sampler_synth_note_on(60, 100, 0, 0));
    assert(!sampler_synth_note_off(60, 0, 0));
    assert(sampler_synth_get_last_error() == SAMPLER_SYNTH_ERROR_QUEUE_FULL);
    finish();
    puts("PASS queue-full reporting and repeated lifecycle");

    reset_capture(); cfg = config(); assert(sampler_synth_init(&cfg));
    inspect_shutdown = true; stop_after = 1;
    assert(sampler_synth_shutdown()); finish();
    reset_capture(); assert(sampler_synth_init(&cfg));
    force_output_failure = true; stop_after = 1;
    assert(!sampler_synth_shutdown());
    assert(failed_blocks == 1 && sampler_synth_get_last_error() == SAMPLER_SYNTH_ERROR_OUTPUT);
    finish();
    puts("PASS shutdown zero-block acknowledgement and output failure propagation");
    return 0;
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 xc-soundfont-synth contributors
#include <stddef.h>
#include <stdint.h>

#include "driver/i2s_std.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sampler_synth.h"
#include "sdkconfig.h"

#define EXAMPLE_RATE 44100U
static const char *TAG = "xc_example";

/**
 * @brief 将临时 PCM 块完整写入标准 I2S TX，以 DMA 背压维持音频节拍。
 * @param samples PCM16 交错立体声；frames 为每声道帧数；ctx 为已启动 TX 句柄。
 * @return true 表示所有字节写入成功，超时或部分写入返回 false。
 * 50 ms 超时保证输出故障时生命周期调用可继续推进；不保留 samples 指针。
 */
static bool write_pcm(const int16_t *samples, size_t frames, void *ctx)
{
    size_t written = 0;
    const size_t bytes = frames * SAMPLER_SYNTH_OUTPUT_CHANNELS * sizeof(int16_t);
    return i2s_channel_write((i2s_chan_handle_t)ctx, samples, bytes, &written, 50) == ESP_OK &&
           written == bytes;
}

/**
 * @brief 由应用挂载 SD 卡；组件只使用 VFS 路径，不持有 SPI 总线和 SD 生命周期。
 * @param card 返回已挂载卡句柄，供应用退出时卸载。
 * @return ESP_OK 表示挂载成功；失败会释放本函数创建的 SPI 总线。
 * 所有引脚由 menuconfig 明确选择，不使用原 OFLU 板的配置。
 */
static esp_err_t mount_sd(sdmmc_card_t **card)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    const spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_XC_SD_MOSI,
        .miso_io_num = CONFIG_XC_SD_MISO,
        .sclk_io_num = CONFIG_XC_SD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(host.slot, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK)
        return err;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = host.slot;
    slot.gpio_cs = CONFIG_XC_SD_CS;
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount, card);
    if (err != ESP_OK)
        spi_bus_free(host.slot);
    return err;
}

/**
 * @brief 创建标准 Philips I2S 的 PCM16 双声道 TX；适用于已配置的外置 I2S DAC。
 * @param tx 返回 TX 句柄，初始化失败时会删除临时句柄。
 * @return ESP_OK 表示 TX 已启用。
 * 该示例不配置 I2C codec、MCLK 或功放；需要这些能力的板由应用补充初始化。
 */
static esp_err_t start_i2s(i2s_chan_handle_t *tx)
{
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    channel.dma_desc_num = 4;
    channel.dma_frame_num = 128;
    esp_err_t err = i2s_new_channel(&channel, tx, NULL);
    if (err != ESP_OK)
        return err;
    const i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EXAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_XC_I2S_BCLK,
            .ws = CONFIG_XC_I2S_WS,
            .dout = CONFIG_XC_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    err = i2s_channel_init_std_mode(*tx, &standard);
    if (err == ESP_OK)
        err = i2s_channel_enable(*tx);
    if (err != ESP_OK)
    {
        i2s_del_channel(*tx);
        *tx = NULL;
    }
    return err;
}

/**
 * @brief 初始化板级 SD/I2S，等待 SF2 异步加载，演示单音和连音，再按顺序退出。
 * 引脚未配置时只记录错误并返回，不驱动默认 GPIO。不会格式化 SD 卡。
 * 仅供接线后的演示验证，不是端到端延迟或通用 codec 适配测试。
 */
void app_main(void)
{
    if (CONFIG_XC_I2S_BCLK < 0 || CONFIG_XC_I2S_WS < 0 || CONFIG_XC_I2S_DOUT < 0 ||
        CONFIG_XC_SD_MOSI < 0 || CONFIG_XC_SD_MISO < 0 || CONFIG_XC_SD_SCLK < 0 || CONFIG_XC_SD_CS < 0)
    {
        ESP_LOGE(TAG, "Configure all GPIOs in menuconfig -> XC SoundFont example first");
        return;
    }
    sdmmc_card_t *card = NULL;
    i2s_chan_handle_t tx = NULL;
    esp_err_t err = mount_sd(&card);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return;
    }
    err = start_i2s(&tx);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(err));
        goto unmount;
    }
    const sampler_synth_cfg_t cfg = {
        .sample_rate = EXAMPLE_RATE,
        .output_write = write_pcm,
        .output_ctx = tx,
    };
    if (!sampler_synth_init(&cfg))
    {
        ESP_LOGE(TAG, "Synth init failed: %s", sampler_synth_error_name(sampler_synth_get_last_error()));
        goto stop_i2s;
    }
    if (!sampler_synth_load_soundfont(0, CONFIG_XC_SF2_PATH))
        goto stop_synth;
    const int64_t wait_start = esp_timer_get_time();
    while (sampler_synth_get_state() == SAMPLER_SYNTH_STATE_LOADING &&
           esp_timer_get_time() - wait_start < 5000000)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (sampler_synth_get_state() != SAMPLER_SYNTH_STATE_READY)
    {
        ESP_LOGE(TAG, "SF2 not ready: %s", sampler_synth_error_name(sampler_synth_get_last_error()));
        goto stop_synth;
    }
    sampler_synth_set_expression(80);
    const int notes[] = {60, 62, 64, 67};
    if (!sampler_synth_note_on(notes[0], 100, 0, 0))
        goto stop_synth;
    vTaskDelay(pdMS_TO_TICKS(600));
    for (size_t i = 1; i < sizeof(notes) / sizeof(notes[0]); ++i)
    {
        if (!sampler_synth_legato_transition(notes[i - 1], notes[i], 100, 0, 0))
            goto stop_synth;
        vTaskDelay(pdMS_TO_TICKS(600));
    }
    if (!sampler_synth_note_off(67, 0, 0))
        ESP_LOGW(TAG, "note off queue rejected; proceeding to shutdown");

stop_synth:
    if (!sampler_synth_shutdown())
        ESP_LOGW(TAG, "Silence submission failed; board must ensure hardware mute");
    sampler_synth_deinit();
    /* 仅针对本例 4x128 DMA 的保守等待；真实产品须按输出后端实现排空/硬件静音。 */
    vTaskDelay(pdMS_TO_TICKS(100));
stop_i2s:
    i2s_channel_disable(tx);
    i2s_del_channel(tx);
unmount:
    esp_vfs_fat_sdcard_unmount("/sdcard", card);
    spi_bus_free(SPI2_HOST);
}

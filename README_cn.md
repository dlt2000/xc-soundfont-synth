# xc-soundfont-synth

[简体中文](README_cn.md) | [English](README.md)

**面向自制电子乐器的 ESP-IDF SoundFont（SF2）音色合成组件。**

如果你正在制作电吹管、电吉他等其他通过传感器控制音高和响度的乐器，
这个组件可以承担从音符事件到 PCM 音频的部分。应用负责读取传感器、决定音高，
组件负责加载音色、渲染声音，并通过回调交给你的 I2S、codec 或其他输出后端。

底层 SF2 解析与采样合成采用[TinySoundFont](https://github.com/schellingb/TinySoundFont)。本仓库补充了 ESP-IDF / FreeRTOS集成、命令调度、表情控制、连音过渡、轻量混响与输出适配接口。

## 能做什么

- 从应用已经挂载的 ESP-IDF VFS 文件系统加载未压缩 `.sf2`，不限于 SD 卡。
- 输出 PCM16 交错立体声，默认 44.1 kHz、每块 128 帧。
- 通过 FreeRTOS 队列串行处理起音、断音、音色加载、混响与静音命令。
- 通过最新值更新 0–127 的线性响度目标，采用约 5 ms 平滑，并补偿 TinySoundFont 的 CC11 曲线。
- 采用旧音淡出、零增益处换音、新音淡入的约 5 ms 连音过渡。
- 提供双延迟反馈式轻量混响，以及可选的渲染追踪与低频诊断日志。
- 配置 SF2 bank/preset、文件大小预算、zone 检查范围、任务优先级、栈、队列与绑核。

当前是**单实例、单音符演奏控制**。`max_voices` 是 SF2 叠层等内部 voice 的资源预算，
不表示可以同时演奏相同数量的独立音符；当前 `note_on()` 会结束先前演奏音，
`note_off()` 会关闭全部演奏通道。键盘和弦、多 MIDI 通道、多音色同时发声需要进一步扩展。

## 组件边界

```text
应用：传感器 / 按键 / MIDI 解析 / 乐器规则
                │ note / expression / legato
                ▼
xc-soundfont-synth
  FreeRTOS 命令队列 → TinySoundFont → 表情与连音 → 混响
                │ PCM16，L/R 交错
                ▼
应用提供的 output_write() → I2S / codec / 其他输出
```

组件不初始化 GPIO、I2C、I2S、codec、功放或 SD 卡，不包含指法、气息采样、UI、蓝牙或 Wi-Fi。
文件系统挂载、音色文件访问互斥、输出设备的采样格式及生命周期都由应用负责。

## 环境与依赖

| 项目     | 要求                                                                    |
| -------- | ----------------------------------------------------------------------- |
| 开发框架 | ESP-IDF；本次构建验证版本为 5.5.1，清单声明范围为 `>=5.5.1,<6.0`        |
| 语言     | C，头文件支持从 C++ 调用                                                |
| 核心依赖 | ESP-IDF 自带的 FreeRTOS、heap、esp_timer、esp_common，以及 C 运行库/VFS |
| 输出     | 应用提供支持 PCM16 双声道的同步提交回调                                 |
| 存储     | 应用已挂载、可通过 `stat()` / `fopen()` 读取的文件系统                  |
| PSRAM    | 不强制要求；较大 SF2 通常需要，分配策略由 ESP-IDF 的堆配置决定          |

本组件不依赖 Arduino、GMF 或 `esp_codec_dev`。使用者仍可在应用的输出回调中调用
`esp_codec_dev_write()`，也可以直接使用 ESP-IDF I2S 驱动。

清单的版本范围不是完整兼容矩阵。其他 ESP-IDF 版本、芯片和输出板需要自行构建与实机验证。
默认任务不绑核；打开绑核时会检查所选核心是否存在。

## 接入已有工程

将本仓库放入 ESP-IDF 工程的 `components` 目录，保持目录名：

```text
your_project/
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   └── main.c
└── components/
    └── xc-soundfont-synth/
        ├── CMakeLists.txt
        ├── idf_component.yml
        ├── include/sampler_synth.h
        ├── sampler_synth.c
        └── tsf.h
```

在调用组件的 `CMakeLists.txt` 中添加依赖：

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES xc-soundfont-synth
)
```

如果应用直接操作 I2S，还需要为应用声明 `esp_driver_i2s` 依赖。
仓库自带 TinySoundFont 源文件，编译时不需要另外下载它。

### 提供 PCM 输出回调

以下示例假设应用已经初始化并启用了 `tx`，格式为 44.1 kHz、16 bit、双声道：

```c
#include "driver/i2s_std.h"
#include "sampler_synth.h"

static bool write_pcm(const int16_t *samples, size_t frames, void *ctx)
{
    size_t written = 0;
    const size_t bytes = frames * 2 * sizeof(int16_t);
    return i2s_channel_write((i2s_chan_handle_t)ctx, samples, bytes,
                            &written, 50) == ESP_OK && written == bytes;
}

/* tx 必须在 sampler_synth_deinit() 返回后才能关闭/释放。 */
static bool start_synth(i2s_chan_handle_t tx)
{
    const sampler_synth_cfg_t cfg = {
        .sample_rate = 44100,
        .output_write = write_pcm,
        .output_ctx = tx,
    };
    return sampler_synth_init(&cfg);
}
```

回调在渲染任务中执行，必须按音频时钟提供背压，并在有限时间内返回。
总是立即成功返回的“丢弃数据”回调会使任务高速空转，不能直接用于实时运行。
返回 `true` 前必须写完或复制整块数据，异步 DMA 不能继续引用组件临时缓冲区。
写入失败返回 `false`，组件记录 `SAMPLER_SYNTH_ERROR_OUTPUT` 并让出一个 tick 后继续尝试。

### 加载与演奏顺序

1. 应用挂载存储并初始化输出硬件，然后调用 `sampler_synth_init()`。
2. 提交 `sampler_synth_load_soundfont(0, "/sdcard/instrument.sf2")`，检查返回值。
3. 从应用控制任务轮询状态，设置有限超时；只有 `READY` 才开始演奏，`ERROR` 时读取错误。
4. 设置非零 expression，例如 `sampler_synth_set_expression(80)`；初始响度为零。
5. 使用 `sampler_synth_note_on(60, 100, 0, 0)` 起音；换音可用 `legato_transition()`。
6. 使用 `sampler_synth_note_off(60, 0, 0)` 断音，并处理入队失败。

`load_soundfont()`、`note_on()`、`note_off()`、`legato_transition()`、`set_reverb()` 返回 `true`
只表示命令成功入队。实际解析、发声和输出仍可能失败，不能把入队结果当作声音已经播出。
完整的超时、错误处理与退出流程见 [I2S＋SD 卡示例](examples/i2s_sdcard/main/main.c)。

## 运行示例

[examples/i2s_sdcard](examples/i2s_sdcard) 包含一个独立 ESP-IDF 工程：
通过 SPI 挂载 FAT SD 卡，将 SF2 加载到内存，再通过标准 Philips I2S 向外置 DAC 输出四个单音。
它不包含 codec 的 I2C 寄存器配置、MCLK 或功放使能，适合不需要这些控制的 I2S DAC，
其他板卡请补充自己的硬件初始化。

```sh
cd examples/i2s_sdcard
idf.py set-target esp32
idf.py menuconfig
idf.py build
```

在 `XC SoundFont example` 中设置 I2S、SD SPI 引脚与 SF2 路径。
所有 GPIO 默认 `-1`，未配置时示例只报告错误，不驱动预设引脚。
将可合法使用、含 bank 0 / preset 0 且覆盖示例音符 60、62、64、67 的 SF2 放到 SD 卡。
示例不会在挂载失败时格式化卡。接线完成、确认引脚与电气条件后再执行：

```sh
idf.py -p <PORT> flash monitor
```

示例默认不强制 PSRAM。使用较大音色前，应在适合目标板的配置中启用并验证 PSRAM，
允许普通 `malloc()` 使用外部 RAM，再按实际可用堆设置资源预算。
不要直接套用其他开发板的 PSRAM、电源或 codec 配置。

## 配置

配置结构体可以零初始化，但 `output_write` 必填。

| 字段                             | 零值行为 / 默认值 | 说明                                                                     |
| -------------------------------- | ----------------- | ------------------------------------------------------------------------ |
| `sample_rate`                    | 44100             | 可配置 8000–192000；仅检查参数范围，不保证任意速率实时可用；输出端须一致 |
| `chunk_frames`                   | 128               | 每声道一块的帧数；改变块大小会影响调度、缓冲和响应                       |
| `max_voices`                     | 8                 | TinySoundFont 内部 voice 预算，不是多音符复音数                          |
| `max_sf2_bytes`                  | 512 KiB           | 默认文件预算；可显式设置更大值，不保证一定能分配成功                     |
| `bank` / `preset`                | 0 / 0             | 初始化时选择 SF2 bank/program；更换选择需重新初始化                      |
| `validate_zones`                 | 关闭              | 开启后在 `zone_velocity` 下检查 `note_min..note_max` 每音至少有一个 zone |
| `zone_velocity`                  | 100               | 检查使用的力度，不能代替所有力度的覆盖检查                               |
| `max_zones_per_note`             | 不限制            | 开启 zone 检查后，可限制同一音符匹配的叠层数量                           |
| `min_free_psram_bytes`           | 不检查            | 可选的加载前空闲 PSRAM 门槛；不控制分配位置，也不保证最大连续块足够      |
| `command_queue_length`           | 16                | 队列满立即返回失败，演奏任务不会等待队列空位                             |
| `task_stack_bytes`               | 6144              | ESP-IDF 的字节单位，显式配置不得低于 4096                                |
| `task_priority`                  | 8                 | 必须小于 `configMAX_PRIORITIES`；值 0 被解释为默认值                     |
| `pin_task_to_core` / `task_core` | 不绑核            | 开启后由调用方选择存在的核心                                             |
| `diagnostics`                    | 关闭              | 开启后约每 10 秒打印渲染、写入、超时和堆统计                             |

SF2 可选择任意可用 preset，默认不强制原 OFLU 的 MIDI 36–71 音域与每音最多两层。
若要保持原音色合同，可设置：

```c
cfg.validate_zones = true;
cfg.note_min = 36;
cfg.note_max = 71;
cfg.zone_velocity = 100;
cfg.max_zones_per_note = 2;
cfg.min_free_psram_bytes = 1152U * 1024U;
```

原固件的 codec/SD 初始化应移到应用，再通过 `output_write` 连接输出。
旧 `SAMPLER_SYNTH_ERROR_CODEC` 数值保留为 `SAMPLER_SYNTH_ERROR_OUTPUT` 的兼容别名。
本次拆分改变了配置结构和初始化要求，原调用方需要重新编译并补充输出回调，不能直接替换二进制。

## 内存、线程与时序

- 使用未压缩 SF2；不支持 SF3 压缩音色。TinySoundFont 尚未实现 SF2 modulators、
  chorus send 和 reverb send 等功能；本组件的轻量混响独立于 SF2 内的效果发送配置，
  不能保证与桌面音源软件听感一致。
- SF2 全量加载，TinySoundFont 会把 PCM16 样本转换为浮点；运行内存通常明显大于文件大小。
  还需要 preset、zone、voice、栈和输出缓冲，文件大小门槛不能作为内存分配成功的保证。
- 44.1 kHz 时，混响的两个 float 延迟缓冲约占 58.6 KiB。
  初始化时尝试分配；失败后基础合成仍可工作，但开启混响会返回 `false`。
- 原组件的单音换音算法及本地 TinySoundFont 增益扩展予以保留。
  叠层 SF2 仍可能使用多个 voice，不能把单音符等同于单个 voice。
- 只有渲染任务操作 TinySoundFont；普通控制命令按队列顺序处理，expression 保留最新值。
  连音过渡中只保留最新后继目标，快速扫过的中间音可能不会发声。
- 控制接口仅供任务上下文调用，不支持 ISR。`init()`、`shutdown()`、`deinit()` 必须由应用串行调用；
  反初始化前先停止全部事件生产者，不得与其他 API 并发释放队列或缓冲。
- 输出回调和追踪回调不能阻塞于 UI、文件或无线业务；追踪回调只允许在未初始化时设置。
- 音色读取和解析在渲染任务中执行，会暂停向输出端提交新 PCM，可能导致 underrun。
  切换前由应用断音/静音，在非演奏期加载；不提供后台预加载或无缝切换。
- 默认 128 帧在 44.1 kHz 下约为 2.90 ms，只是一块音频的时长，不是传感器到扬声器的总延迟。
  约 5 ms 的表情/连音参数同样不是端到端延迟测量。

### 停止与关机

推荐顺序：停止事件生产者 → `sampler_synth_shutdown()` → `sampler_synth_deinit()` →
由应用完成输出排空、codec/功放静音及硬件关闭。

`shutdown()` 最多等待约 1 秒，成功表示一块全零 PCM 已被回调接受，**不表示 DMA 已经播放到零块**。
若失败，应用仍须完成硬件静音。`deinit()` 会等待正在执行的回调或文件操作结束，
因此输出必须设置有限超时，且应用必须避免并发修改正在加载的 SF2 文件。

## 源码结构

```text
include/sampler_synth.h       公共接口、配置和回调约定
sampler_synth.c               任务、命令、SF2 加载、演奏控制与混响
tsf.h                        带本地扩展的 TinySoundFont
TINYSOUNDFONT_VERSION.md      上游提交、实际哈希和修改说明
THIRD_PARTY_NOTICES.md        第三方项目与许可证声明
LICENSE                      本项目 MIT 许可证
LICENSES/                    随附第三方许可全文
examples/i2s_sdcard/          可独立构建的接入示例
tests/                       主机侧回归验证与测试说明
```

## 验证状态与贡献

自动验证结果记录在 [验证说明](tests/README.md)。固件构建只证明代码可编译链接，
不代表已经完成特定 DAC、codec 或开发板的音频测试。
实际接入时应验证冷启动、弱/强表情、快速换音、起断音、音色加载失败、队列压力、
输出超时、关机静音，以及所用后台任务运行时是否发生卡音、爆音或 watchdog。

欢迎提交 Issue 和 Pull Request。复现问题时请附 ESP-IDF 版本、芯片、输出后端、
关键配置、错误码与复现步骤。提交音色或录音前请确认具有分发权限；
性能数据请说明测量端点、采样方法与硬件条件。

## 开源协议与致谢

本项目自有代码与文档采用 **[MIT License](LICENSE)**，允许在保留版权与许可声明的条件下使用、
修改、分发和商业使用；软件按现状提供，完整条件以许可原文为准。

- **[TinySoundFont](https://github.com/schellingb/TinySoundFont)**：由 Bernhard Schelling 维护，
  基于 Steve Folta 的 SFZero，采用 MIT License。它负责 SF2 解析、采样重放、包络、滤波与基础合成。
  本仓库的 `tsf.h` 包含本地混音增益与 voice 回收扩展，保留原作者版权和许可；
  不是未经修改的上游文件，详见 [版本说明](TINYSOUNDFONT_VERSION.md)。
- **[ESP-IDF](https://github.com/espressif/esp-idf)**：提供底层运行环境与驱动接口，
  框架主要采用 Apache-2.0，所含第三方组件按各自许可证分发。
  FreeRTOS 由 ESP-IDF 提供，其内核采用 MIT License；此仓库不再分发 SDK 源码。

完整的来源、版权与许可范围见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
本项目不附带 SF2 音色文件。组件的 MIT 许可证不覆盖外部音色、采样、商标或接入的其他库，
使用及分发音色时须遵守音色作者的授权条件。

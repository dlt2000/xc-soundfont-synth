# xc-soundfont-synth

[简体中文](README_cn.md) | [English](README.md)

**A SoundFont (SF2) synthesis component for DIY electronic instruments, built on ESP-IDF.**

For electronic wind instruments, guitars, and other projects that use sensors to control pitch and loudness,
this component handles the path from note events to PCM audio. Your application reads sensors and determines
the pitch; the component loads SoundFonts, renders audio, and passes it to your I2S, codec, or other output
backend through a callback.

SF2 parsing and sample synthesis are provided by [TinySoundFont](https://github.com/schellingb/TinySoundFont).
This repository adds ESP-IDF / FreeRTOS integration, command scheduling, expression control, legato transitions,
lightweight reverb, and an output adapter interface.

## Features

- Load uncompressed `.sf2` files from a filesystem already mounted through ESP-IDF VFS; storage is not limited to SD cards.
- Render interleaved stereo PCM16, with defaults of 44.1 kHz and 128 frames per block.
- Serialize note-on, note-off, SoundFont loading, reverb, and silence commands through a FreeRTOS queue.
- Update a linear loudness target from 0 to 127 using the latest value, with approximately 5 ms smoothing and compensation for TinySoundFont's CC11 curve.
- Perform approximately 5 ms legato transitions by fading out the old note, switching at zero gain, and fading in the new note.
- Provide lightweight reverb using two feedback delay buffers, with optional render tracing and periodic diagnostic logging.
- Configure the SF2 bank/preset, file size budget, zone validation range, task priority, stack, queue, and core affinity.

The current implementation supports **one instance and monophonic note control**. `max_voices` sets the budget
for internal SF2 voices, including sample layers; it is not the number of independent notes you can play
simultaneously. `note_on()` ends the previous performance note, and `note_off()` stops all performance channels.
Keyboard chords, multiple MIDI channels, and simultaneous instruments require further development.

## Component boundaries

```text
Application: sensors / buttons / MIDI parsing / instrument logic
                │ note / expression / legato
                ▼
xc-soundfont-synth
  FreeRTOS command queue → TinySoundFont → expression & legato → reverb
                │ PCM16, interleaved L/R
                ▼
Application-provided output_write() → I2S / codec / other output
```

The component does not initialize GPIO, I2C, I2S, codecs, amplifiers, or SD cards. It does not include fingering
logic, breath sampling, a UI, Bluetooth, or Wi-Fi. Your application owns filesystem mounting, synchronization
of SoundFont file access, and the output device's sample format and lifecycle.

## Requirements and dependencies

| Item              | Requirement                                                                                               |
| ----------------- | --------------------------------------------------------------------------------------------------------- |
| Framework         | ESP-IDF; build validation used 5.5.1. The manifest declares `>=5.5.1,<6.0`.                               |
| Language          | C, with public headers callable from C++                                                                  |
| Core dependencies | FreeRTOS, heap, esp_timer, and esp_common from ESP-IDF, plus the C runtime/VFS                            |
| Output            | An application-provided callback that synchronously submits stereo PCM16                                  |
| Storage           | A mounted filesystem accessible through `stat()` / `fopen()`                                              |
| PSRAM             | Optional; usually needed for larger SF2 files. Allocation behavior depends on ESP-IDF heap configuration. |

The component does not depend on Arduino, GMF, or `esp_codec_dev`. Applications can still call
`esp_codec_dev_write()` in their output callback, or use the ESP-IDF I2S driver directly.

The manifest's version range is not a complete compatibility matrix. Other ESP-IDF versions, chips, and
output boards require their own builds and hardware validation. The render task is not pinned to a core by
default; when pinning is enabled, initialization checks that the selected core exists.

## Integrating into an existing project

Place this repository under your ESP-IDF project's `components` directory, keeping the directory name shown below:

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

Add the dependency to the calling component's `CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES xc-soundfont-synth
)
```

If your application calls the I2S driver directly, also declare an `esp_driver_i2s` dependency for that application
component. TinySoundFont source is included in this repository and does not need to be downloaded separately
at build time.

### Providing a PCM output callback

The following example assumes that your application has already initialized and enabled `tx` for 44.1 kHz,
16-bit stereo output:

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

/* Do not close or release tx until sampler_synth_deinit() has returned. */
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

The callback runs in the render task. It must provide backpressure paced by the audio clock and return within
a bounded time. A callback that discards data and immediately returns success will make the task spin at full
speed and is not suitable for real-time operation as-is.

Before returning `true`, the callback must write or copy the entire block. Asynchronous DMA must not retain
a reference to the component's temporary buffer. Return `false` on a write failure; the component records
`SAMPLER_SYNTH_ERROR_OUTPUT`, yields for one tick, and then continues trying to submit audio.

### Loading and playing

1. Mount storage and initialize the output hardware in your application, then call `sampler_synth_init()`.
2. Submit `sampler_synth_load_soundfont(0, "/sdcard/instrument.sf2")` and check the return value.
3. Poll the state from your application's control task with a finite timeout. Play only after `READY`; read the error if the state becomes `ERROR`.
4. Set a nonzero expression value, for example `sampler_synth_set_expression(80)`. The initial loudness is zero.
5. Start a note with `sampler_synth_note_on(60, 100, 0, 0)`. Use `legato_transition()` to transition to another note.
6. Stop the note with `sampler_synth_note_off(60, 0, 0)` and handle any queue submission failure.

A `true` result from `load_soundfont()`, `note_on()`, `note_off()`, `legato_transition()`, or `set_reverb()`
only means that the command was queued successfully. Parsing, voice creation, or audio output can still fail;
a successful queue submission does not mean that audio has already played.

See the [I2S + SD card example](examples/i2s_sdcard/main/main.c) for a complete timeout, error-handling,
and cleanup sequence.

## Running the example

[examples/i2s_sdcard](examples/i2s_sdcard) is a standalone ESP-IDF project. It mounts a FAT SD card over SPI,
loads an SF2 file into memory, and plays four successive notes through standard Philips I2S to an external DAC.
It does not configure codec I2C registers, MCLK, or amplifier enable signals, so it is intended for I2S DACs that
do not require those controls. Add the appropriate hardware initialization for other boards.

```sh
cd examples/i2s_sdcard
idf.py set-target esp32
idf.py menuconfig
idf.py build
```

Configure the I2S pins, SD SPI pins, and SF2 path under `XC SoundFont example`. All GPIO settings default to
`-1`; until configured, the example reports an error instead of driving preset pins.

Place an SF2 file that you are authorized to use on the SD card. It must contain bank 0 / preset 0 and cover
the example notes 60, 62, 64, and 67. The example does not format the card if mounting fails. After wiring
the hardware and checking the pins and electrical requirements, run:

```sh
idf.py -p <PORT> flash monitor
```

The example does not require PSRAM by default. Before using a larger SoundFont, enable and verify PSRAM
using settings appropriate for your board, allow ordinary `malloc()` allocations to use external RAM,
and choose a resource budget based on the available heap. Do not copy another board's PSRAM, power,
or codec configuration without checking it against your hardware.

## Configuration

The configuration structure can be zero-initialized, but `output_write` is required.

| Field                            | Zero-value behavior / default | Description                                                                                                                                         |
| -------------------------------- | ----------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| `sample_rate`                    | 44100                         | Accepts 8000–192000. This is parameter validation, not a guarantee of real-time operation at every rate. The output backend must use the same rate. |
| `chunk_frames`                   | 128                           | Frames per channel per block. Changing this affects scheduling, buffering, and response.                                                            |
| `max_voices`                     | 8                             | TinySoundFont's internal voice budget, not the number of independently playable notes.                                                              |
| `max_sf2_bytes`                  | 512 KiB                       | Default file size budget. A larger value can be set explicitly, but allocation can still fail.                                                      |
| `bank` / `preset`                | 0 / 0                         | Selects the SF2 bank/program at initialization. Changing the selection requires reinitialization.                                                   |
| `validate_zones`                 | Disabled                      | When enabled, checks that every note in `note_min..note_max` has at least one zone at `zone_velocity`.                                              |
| `zone_velocity`                  | 100                           | Velocity used for zone validation; it does not validate coverage at every velocity.                                                                 |
| `max_zones_per_note`             | Unlimited                     | Optional limit on matching sample layers per note when zone validation is enabled.                                                                  |
| `min_free_psram_bytes`           | No check                      | Optional free-PSRAM threshold before loading. It does not control allocation placement or guarantee a sufficiently large contiguous block.          |
| `command_queue_length`           | 16                            | Queue submission fails immediately when full; the calling performance task does not wait for space.                                                 |
| `task_stack_bytes`               | 6144                          | Stack size in ESP-IDF bytes. An explicit value must be at least 4096.                                                                               |
| `task_priority`                  | 8                             | Must be below `configMAX_PRIORITIES`; zero selects the default.                                                                                     |
| `pin_task_to_core` / `task_core` | Not pinned                    | When enabled, the application must select an existing core.                                                                                         |
| `diagnostics`                    | Disabled                      | When enabled, logs render, write, timeout, and heap statistics approximately every 10 seconds.                                                      |

Any available SF2 preset can be selected. By default, the component does not enforce the original OFLU
requirements of MIDI notes 36–71 and at most two layers per note. To preserve those validation requirements, use:

```c
cfg.validate_zones = true;
cfg.note_min = 36;
cfg.note_max = 71;
cfg.zone_velocity = 100;
cfg.max_zones_per_note = 2;
cfg.min_free_psram_bytes = 1152U * 1024U;
```

Move codec/SD initialization from the original firmware into your application and connect the output through
`output_write`. The old `SAMPLER_SYNTH_ERROR_CODEC` value is retained as a compatibility alias for
`SAMPLER_SYNTH_ERROR_OUTPUT`.

This extraction changes the configuration structure and initialization requirements. Existing callers must
provide an output callback and recompile; this is not a binary-compatible replacement.

## Memory, threading, and timing

- Use uncompressed SF2 files; compressed SF3 files are not supported. TinySoundFont does not yet implement
  SF2 modulators, chorus send, or reverb send. This component's lightweight reverb is independent of the
  effect-send settings inside the SF2 file, so playback is not guaranteed to sound the same as a desktop synthesizer.
- SF2 files are loaded in full, and TinySoundFont converts PCM16 samples to floating point. Runtime memory
  usage is usually considerably larger than the file size. Presets, zones, voices, stacks, and output buffers
  also consume memory; passing the file size check does not guarantee successful allocation.
- At 44.1 kHz, the two floating-point reverb delay buffers use approximately 58.6 KiB. Initialization attempts
  to allocate them. Basic synthesis remains available if allocation fails, but enabling reverb will return `false`.
- The original component's monophonic note-transition algorithm and local TinySoundFont gain extensions
  are retained. Layered SoundFonts can still use multiple voices; one note does not necessarily mean one voice.
- Only the render task accesses TinySoundFont. Ordinary control commands are processed in queue order,
  while expression retains the latest value. During a legato transition, only the latest pending target is
  retained, so rapidly traversed intermediate notes may not sound.
- Control APIs are task-context APIs and cannot be called from an ISR. The application must serialize
  `init()`, `shutdown()`, and `deinit()`. Stop all event producers before deinitialization; do not release
  queues or buffers concurrently with other API calls.
- Output and trace callbacks must not block on UI, file, or wireless operations. Set the trace callback only
  while the synthesizer is not initialized.
- SoundFont reading and parsing run in the render task and pause new PCM submissions, potentially causing
  an underrun. Stop notes or mute output in the application before loading, and switch SoundFonts outside
  active performance. Background preloading and seamless switching are not provided.
- At 44.1 kHz, the default 128-frame block represents approximately 2.90 ms of audio. This is a block duration,
  not sensor-to-speaker latency. The approximately 5 ms expression and legato settings are not end-to-end
  latency measurements either.

### Stopping and shutting down

Recommended order: stop event producers → `sampler_synth_shutdown()` → `sampler_synth_deinit()` →
drain the output backend, mute the codec/amplifier, and shut down hardware in the application.

`shutdown()` waits for approximately one second at most. Success means that the output callback has accepted
a block of zero PCM; **it does not mean that DMA has finished playing through that block**. If it fails, the
application must still ensure hardware mute. `deinit()` waits for an in-progress callback or file operation
to finish, so output writes must use finite timeouts, and the application must prevent concurrent changes
to an SF2 file being loaded.

## Repository layout

```text
include/sampler_synth.h       Public API, configuration, and callback contracts
sampler_synth.c               Task, commands, SF2 loading, performance control, and reverb
tsf.h                        TinySoundFont with local extensions
TINYSOUNDFONT_VERSION.md      Upstream commit, actual hashes, and local modifications
THIRD_PARTY_NOTICES.md        Third-party projects and license notices
LICENSE                      MIT license for this project
LICENSES/                    Full copies of bundled third-party licenses
examples/i2s_sdcard/          Standalone integration example
tests/                       Host regression checks and validation notes
```

## Validation and contributing

Automated validation results are recorded in the [validation notes](tests/README.md) (Chinese). A successful
firmware build demonstrates compilation and linking; it does not establish audio behavior on a particular
DAC, codec, or board.

When integrating the component, verify cold startup, weak/strong expression, rapid note transitions,
note-on/off behavior, failed SoundFont loads, queue pressure, output timeouts, and shutdown silence.
Also check for stuck notes, clicks, or watchdog resets while other application tasks are running.

Issues and pull requests are welcome. For bug reports, include the ESP-IDF version, chip, output backend,
relevant configuration, error codes, and reproduction steps. Confirm that you have distribution rights before
submitting SoundFonts or recordings. For performance results, describe the measurement endpoints,
sampling method, and hardware conditions.

## License and acknowledgments

Original code and documentation in this project are licensed under the **[MIT License](LICENSE)**.
Use, modification, redistribution, and commercial use are permitted subject to retaining the required copyright
and license notices. The software is provided as-is; the full license text governs the terms.

- **[TinySoundFont](https://github.com/schellingb/TinySoundFont)**, maintained by Bernhard Schelling and based
  on Steve Folta's SFZero, is licensed under MIT. It provides SF2 parsing, sample playback, envelopes,
  filtering, and basic synthesis. This repository's `tsf.h` includes local mixer-gain and voice-reclamation
  extensions and retains the original copyright and license notices. It is not an unmodified upstream file;
  see the [version notes](TINYSOUNDFONT_VERSION.md) (Chinese).
- **[ESP-IDF](https://github.com/espressif/esp-idf)** provides the runtime environment and driver interfaces.
  The framework is primarily licensed under Apache-2.0, with bundled third-party components distributed
  under their respective licenses. FreeRTOS is supplied through ESP-IDF, and its kernel is MIT-licensed.
  This repository does not redistribute the SDK source code.

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) (Chinese) for sources, copyrights, and license scope.
No SF2 SoundFonts are bundled. The component's MIT license does not cover external SoundFonts, samples,
trademarks, or other libraries used by your application. Follow the SoundFont author's license when using
or redistributing a SoundFont.

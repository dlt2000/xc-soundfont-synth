# 验证说明

## 主机侧回归

需要 Python 3 和本机 clang/gcc 工具链。Windows 下的 clang 还需可用的 MSVC / Windows SDK 链接环境。
从仓库根目录运行：

```sh
python tests/run_host_tests.py
```

也可以显式选择编译器，例如 PowerShell：

```powershell
python tests/run_host_tests.py --cc 'C:\Program Files\LLVM\bin\clang.exe'
```

测试编译真实的 `sampler_synth.c` 和随附 `tsf.h`，运行时生成一个简单正弦波 SF2，
不下载或分发第三方音色。ESP-IDF / FreeRTOS API 使用确定性主机替身；
队列和任务在同一主机线程中调度，不模拟中断、双核竞争、DMA 或真实音频时钟。

2026-10-24 在 Windows、Python 3.11、clang 22.1.7 环境下通过以下 6 组检查：

1. 参数校验及单核配置下拒绝无效绑核。
2. 队列、信号量、输出缓冲、任务创建失败时的资源回收。
3. 真实 SF2 解析与非零 PCM 输出、表情、连音、混响、断音控制路径。
4. bank/preset 选择；无效 preset、音域检查、文件预算、PSRAM 门槛、无效文件的静音失败路径。
5. 满队列立即报告失败及重复初始化/反初始化。
6. shutdown 零块确认与输出回调失败向调用方传播。

这些检查不证明音质、实时调度、没有数据竞争或实际板上的关机无爆音。

## ESP-IDF 构建

2026-10-24 使用 ESP-IDF 5.5.1、Xtensa GCC 14.2.0，对 `examples/i2s_sdcard` 的 ESP32 目标执行
完整构建并成功生成应用与 bootloader。该构建不依赖 GMF、`esp_codec_dev` 或原 OFLU 工程。

复现方式：

```sh
cd examples/i2s_sdcard
idf.py set-target esp32
idf.py build
```

GPIO 默认未配置，构建本身不需要接线或附带 SF2。没有执行烧录和实机演奏验证。

## 需要在实际设备验证

- 存储挂载、选定 bank/preset 和目标音域是否符合音色。
- 内部堆、PSRAM 最大连续块、任务栈与 voice 预算是否满足配置。
- 弱/强 expression、起音、断音、连续快速连音和混响是否符合预期。
- 输出超时、队列满、SF2 缺失/损坏、加载失败后的静音与恢复。
- 反复初始化、音色更换和关机时是否发生卡音、爆音、资源泄漏或 watchdog。
- 实际 I2S/codec/功放的排空和静音顺序；回调接受零块不代表 DAC 已播放到该块。

记录性能时应写明测量端点、输出后端、音色和统计方式。
128 帧时长、连音参数和主机测试耗时均不能作为端到端发声延迟。

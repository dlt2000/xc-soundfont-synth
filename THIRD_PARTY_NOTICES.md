# 第三方开源项目声明

本文件说明随仓库分发的第三方代码与运行依赖。项目自有代码和文档采用根目录
[LICENSE](LICENSE) 中的 MIT License；第三方代码保留其原始版权和许可条件。

## TinySoundFont（随仓库分发）

- 项目：<https://github.com/schellingb/TinySoundFont>
- 作者：Bernhard Schelling。
- 来源基线：`853a0a171759f1ddba0de1442133a75912bbeffa`，标记版本 v0.9。
- 本地文件：[`tsf.h`](tsf.h)，包含本地扩展，详情与实际哈希见 [版本说明](TINYSOUNDFONT_VERSION.md)。
- 用途：SoundFont 2 解析、样本加载、采样重放、包络、滤波、通道控制与 PCM 合成。
- 协议：MIT License。
- 原始文件头版权：`Copyright (C) 2017-2025 Bernhard Schelling`。
- 继承声明：`Based on SFZero, Copyright (C) 2012 Steve Folta`，
  原项目为 <https://github.com/stevefolta/SFZero>。
- 许可全文：[文件头原文](LICENSES/TinySoundFont-header-MIT.txt)、
  [固定提交的独立 LICENSE](LICENSES/TinySoundFont-MIT.txt)。两者原始版权年份不同，均按来源保留。

本项目没有将上述基础合成能力归为自行实现。对 TinySoundFont 的通道后级混音增益、
逐采样增益过渡和 voice 回收扩展为本地修改，继续遵循 MIT 许可。
本仓库没有另外复制 SFZero 源码，保留其署名是 TinySoundFont 的来源继承声明。

## ESP-IDF 与 FreeRTOS（外部运行依赖）

- ESP-IDF：<https://github.com/espressif/esp-idf>，由 Espressif Systems 维护。
- 用途：构建系统、FreeRTOS 运行环境、内存分配、定时器，以及示例使用的 I2S、SPI、SD/VFS 接口。
- ESP-IDF 框架主要使用 Apache License 2.0，内含第三方项目遵循各自许可；
  应以所使用版本的 [LICENSE](https://github.com/espressif/esp-idf/blob/v5.5.1/LICENSE)
  和具体组件文件为准。
- FreeRTOS Kernel：<https://github.com/FreeRTOS/FreeRTOS-Kernel>，采用 MIT License。
  本组件使用 ESP-IDF 提供和适配的内核，以 SDK 内相应的版权和许可文件为准。

本仓库不包含 ESP-IDF 或 FreeRTOS 源码副本，不通过本项目的 MIT 许可重新授权 SDK。
若发布包含这些依赖的固件或二进制，应同时保留实际使用依赖所要求的许可与版权声明。

## 可选后端与音色资源

解耦后的组件不包含 GMF、`esp_codec_dev`、具体 codec 驱动或 OFLU 板级初始化代码。
应用如选用这些后端，应自行声明对应依赖并遵守其许可证。

本仓库不附带第三方 SF2、采样或录音。SF2 文件格式本身不能证明具体音色资源可自由分发。
组件代码采用 MIT，不改变音色作者的版权或使用条件；请分别确认音色的使用、修改与分发授权。

测试运行时生成的简短合成波形仅用于回归验证，不来自第三方音色库。

# TinySoundFont 来源与本地修改

- 上游：[schellingb/TinySoundFont](https://github.com/schellingb/TinySoundFont)
- 基线提交：[`853a0a171759f1ddba0de1442133a75912bbeffa`](https://github.com/schellingb/TinySoundFont/tree/853a0a171759f1ddba0de1442133a75912bbeffa)
- 上游文件标记版本：`v0.9`
- 上游原始 `tsf.h` SHA-256：`70D55963C98F60EBB81518EAA1F25D46888D5180EB5F5289FD6B74FFC177D197`
- 本仓库实际 `tsf.h` SHA-256：`1C4C46B7B3CF112A951D3B107D37D819D6EFF4F497D0D13433516A877EB1D3DD`
- 本次重新下载固定提交并比较：2026-10-24

本地文件来自 OFLU 固件，包含以下相对于固定上游提交的扩展，本次解耦没有修改该文件：

1. `tsf_channel_set_mix_volume()`：设置通道后级线性增益目标。
2. `mixVolume` / `mixVolumeTarget` / `mixVolumeStep`：在渲染块内逐采样过渡，稳态使用固定增益路径。
3. `tsf_render_float()` 与各输出模式的 voice 混音扩展：在渲染过程中应用并推进通道后级增益。
4. `tsf_channel_kill_all()`：立即回收指定通道的 voice，供应用控制的零增益切换使用。

这些扩展是本地维护内容，不是上游公开 API；升级时必须合并并回归，不能直接用上游 `tsf.h` 覆盖。
保留的本地实现支持本组件的表情/连音路径，基础 SF2 解析与合成能力来自 TinySoundFont。

文件头保留上游 MIT 许可与 2017–2025 版权；上游独立 `LICENSE` 写的是 2017–2023。
两份原文分别保存在 [LICENSES/TinySoundFont-header-MIT.txt](LICENSES/TinySoundFont-header-MIT.txt)
和 [LICENSES/TinySoundFont-MIT.txt](LICENSES/TinySoundFont-MIT.txt)，未替换原作者署名。
源码随仓库分发，因此编译时不需要在线获取 TinySoundFont。

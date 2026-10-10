# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## 0.5.0 (2026-09-24)

### Changed
- 版本对齐整体 0.5.0 发版；首个公开发版，内容与 0.1.0 一致

## 0.1.0 (2026-09-21)

### Added
- Whisper small 语音识别 ROS 2 独立节点（`whisper_asr_node`）
- 支持 s16le PCM 音频输入（默认 16 kHz 单声道）
- 基于 `libta_llm`（`libacuityllm_rt`）的推理引擎封装
- 三种运行模式：
  - file 离线模式：CLI 转写单个 PCM 文件
  - 单节点在线模式：订阅 `/audio_raw` 话题
  - 联合测试模式：音频源节点 + 推理节点联调
- 麦克风实时采集支持（`mic_source_node.py`）
  - 使用 `arecord` 采集立体声音频
  - 支持差分混音模式（`difference`/`left`/`right`/`average`）
- 音频源节点（`audio_source_node.py`）
  - 支持 WAV/PCM 文件实时回放
  - 可循环播放（`--loop`）
  - 启动延迟等待模型加载（`--start-delay`）
- 分段转写策略：
  - 静默断句（`audio.idle_flush_seconds`，默认 1.0s）
  - 最长窗口兜底（`audio.segment_seconds`，默认 30.0s）
  - 独立推理线程避免阻塞音频接收
- 静音检测与保护：
  - 整段 RMS 低于阈值跳过推理（`audio.silence_rms_threshold`）
  - 积压缓冲超限丢弃最旧音频（`audio.max_buffer_seconds`）
- 幻觉过滤：
  - 检测字符重复模式（如 "君君君君..."）
  - 跳过空白/全空格输出
- 性能统计：
  - RTF（Real-Time Factor）计算
  - 端到端延迟追踪（首采样到文本发布）
  - P50/P90/P99 百分位统计
- 模型自动下载脚本（`download_models.sh` / `ensure_models.sh`）
- gtest 单元测试（`test_whisper_audio.cpp`）

### Configuration
- Whisper 参数：`asr.task`（transcribe/translate）、`asr.language`（默认 zh）
- 音频参数：采样率、声道数、分段策略、静音阈值
- 模型清单：`config/models_manifest.yaml`（支持单核/双核模型）

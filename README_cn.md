# topsbot_whisper_asr

Whisper small 语音识别（ASR）包。调用板端 `libta_llm`（基于 `libacuityllm_rt` 的高层封装）完成推理，输入为 s16le PCM（默认 16 kHz 单声道，见 `audio.sample_rate`/`audio.channels`），输出为中文字幕文本，不依赖 `topsbot_ai_infer`/`pluginlib`。

## 组件

| 目标 | 说明 |
|------|------|
| `topsbot_whisper_utils` | s16le PCM → 归一化 double 的纯逻辑（可脱离板端单测） |
| `topsbot_whisper_engine` | `libta_llm`（`libacuityllm_rt`）引擎的 RAII 封装（load → transcribe） |
| `whisper_asr_node` | 独立 ROS 2 节点：订阅音频 → 分段转写 → 发布文本 |
| `whisper_file_cli` | 离线转写单条 s16le PCM 文件的命令（等价于 `acuity-llm-asr-cli`） |

## 约定

- `model.path` 指向裁剪产物 **`engine_strip.json`**（不是单个 .nb；还需同目录的 `whisper_strip.alm`、`tokenizer.json` 等兄弟文件，见 `config/models_manifest.yaml`）。
- Whisper 选项通过节点参数传递：
  - `asr.task`：`transcribe`（默认）或 `translate`
  - `asr.language`：2 字母代码（默认 `zh`），留空则自动检测
  - `asr.prompt`：初始提示词（`libta_llm` 暂未暴露，预留）
- 音频流约定：话题内容为 s16le 交错 PCM 字节流（`std_msgs/msg/Int8MultiArray`），采样率与声道数由 `audio.sample_rate` / `audio.channels` 配置，需与模型一致。
- 分段策略：音频在**独立推理线程**上按句切段——静默超过 `audio.idle_flush_seconds`（默认 1.0 s）即断句送推理；`audio.segment_seconds`（默认 30.0 s）只是连续语音的兜底最长窗。板上实测单次推理 ≈ 10 s 固定编码开销 + ~0.2 s/输出 token，与输入时长基本无关（2 s 短段 RTF≈6，30 s 长段 RTF≈0.6），所以**长段吞吐最优、短切段纯亏**，一句话整段转写、一条消息完整输出。
- 静音与积压保护：整段 RMS 低于 `audio.silence_rms_threshold`（默认 0.005）视为静音直接跳过推理（省空转、消 " m" 式幻觉）；缓冲超过 `audio.max_buffer_seconds`（默认 120 s）时丢弃最旧音频并告警。`asr.language` 默认为 `zh`——实测英文音频强制 `zh` 会让解码坍缩成单字符幻觉，建议留空自动检测。
- 联合测试回放：`audio_source_node.py` 逐块实时回放（每块 sleep 一个 `chunk_seconds`，不加速），并在开播前等待 `audio.start_delay_seconds`（默认 5.0 s）让 asr 节点加载完模型，避免开头音频被丢。

## 运行模式

### 1. file 离线模式（无 ROS 图，直接跑 CLI）

```bash
# 转写一条 PCM（默认模型路径 ~/topsbot/models/whisper_small/engine_strip.json）
ros2 run topsbot_whisper_asr whisper_file_cli /path/to/input.pcm \
  --model-path ~/topsbot/models/whisper_small/engine_strip.json
```

或经 launch（内部执行 `whisper_file_cli`）：

```bash
ros2 launch topsbot_whisper_asr file_whisper.launch.py \
  pcm_file:=/path/to/input.pcm
```

### 2. 单节点在线模式（audio_raw → text）

```bash
ros2 launch topsbot_whisper_asr whisper.launch.py
# 该模式订阅 /audio_raw 话题，等待其他节点发布音频流
# 例如配合自定义音频采集节点使用
# 验证转写结果：
#   ros2 topic echo /asr_text
```

### 3. 联合测试模式（音频源节点 + 推理节点，见 scripts/audio_source_node.py）

```bash
ros2 launch topsbot_whisper_asr joint_test.launch.py \
  audio_file:=~/topsbot/models/whisper_small/output.wav
```

麦克风实时输入（见 scripts/mic_source_node.py，用 `arecord` 采集）：

```bash
ros2 launch topsbot_whisper_asr mic.launch.py
```

## 模型准备

模型从 `model-zoo/samples/Whisper` 提供的阿里云盘下载，URL 与产物清单见 `config/models_manifest.yaml`。单核 int8 模型（`models_zip_url`）为默认；双核组合模式用 `models_zip_url_2core`（`model-zoo` 里另有该 URL，本仓默认注释）。

> 产物入口是裁剪后的 **`engine_strip.json`**（`model-zoo` README 3.1 节清单），同目录需有 `whisper_strip.alm`、`tokenizer.json`、`vocab.json`、`merges.txt`。

下载脚本（manifest 驱动，解析阿里云盘分享链）：

```bash
bash scripts/download_models.sh          # 全量下载
bash scripts/ensure_models.sh            # 缺失才下载
```

## 编译（交叉）

在 `topsbot_cc` 容器内：

```bash
topsbot_ws/tools/build.sh -s 'topsbot_whisper_asr'
```

核上依赖 `libta_llm.so` / `libacuityllm_rt.so` 已随 sysroot 安装在 `/usr/local/lib`。

## 测试

包内 gtest 快速验证（纯逻辑，PC/板端均可）：

```bash
colcon test --packages-select topsbot_whisper_asr
colcon test-result --verbose
```

## 许可证

Apache License 2.0 — 详见 [LICENSE](./LICENSE)。

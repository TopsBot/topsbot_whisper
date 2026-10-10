# topsbot_whisper_asr

Whisper small ASR (Automatic Speech Recognition) package. Uses on-board `libta_llm` 
(high-level wrapper around `libacuityllm_rt`) for inference. Input is s16le PCM 
(default 16 kHz mono, see `audio.sample_rate`/`audio.channels`), output is Chinese 
subtitle text. Does not depend on `topsbot_ai_infer`/`pluginlib`.

[中文文档](README_cn.md)

## Components

| Target | Description |
|--------|-------------|
| `topsbot_whisper_utils` | Pure logic for s16le PCM → normalized double (testable off-board) |
| `topsbot_whisper_engine` | RAII wrapper around `libta_llm` (`libacuityllm_rt`) engine (load → transcribe) |
| `whisper_asr_node` | Standalone ROS 2 node: subscribes audio → segments → publishes text |
| `whisper_file_cli` | Offline CLI to transcribe a single s16le PCM file (equivalent to `acuity-llm-asr-cli`) |

## Conventions

- `model.path` points to the stripped artifact **`engine_strip.json`** (not a single .nb; 
  also requires sibling files in the same directory: `whisper_strip.alm`, `tokenizer.json`, 
  etc., see `config/models_manifest.yaml`).
- Whisper options are passed via node parameters:
  - `asr.task`: `transcribe` (default) or `translate`
  - `asr.language`: 2-letter code (default `zh`), leave empty for auto-detection
  - `asr.prompt`: initial prompt (not yet exposed by `libta_llm`, reserved)
- Audio stream convention: topic content is s16le interleaved PCM byte stream 
  (`std_msgs/msg/Int8MultiArray`), sample rate and channel count configured via 
  `audio.sample_rate` / `audio.channels`, must match the model.
- Segmentation strategy: audio is segmented by sentence on a **dedicated inference thread** 
  — silence exceeding `audio.idle_flush_seconds` (default 1.0 s) triggers inference; 
  `audio.segment_seconds` (default 30.0 s) is only a fallback cap for continuous speech. 
  On-board testing shows single inference ≈ 10 s fixed encoding overhead + ~0.2 s/output token, 
  largely independent of input duration (2 s short segment RTF≈6, 30 s long segment RTF≈0.6), 
  so **long segments are throughput-optimal, short segments are pure loss** — transcribe 
  one sentence as a whole segment, output one complete message.
- Silence and backlog protection: segments with RMS below `audio.silence_rms_threshold` 
  (default 0.005) are treated as silence and skipped (saves idle inference, eliminates 
  " m"-style hallucinations); when buffer exceeds `audio.max_buffer_seconds` (default 120 s), 
  oldest audio is discarded with a warning. `asr.language` defaults to `zh` — testing shows 
  forcing `zh` on English audio causes decoding collapse into single-character hallucinations; 
  leave empty for auto-detection.
- Joint test playback: `audio_source_node.py` replays audio chunks in real time (sleeps 
  one `chunk_seconds` per chunk, no speed-up), waits `audio.start_delay_seconds` (default 5.0 s) 
  before streaming to let the ASR node load the model, avoiding dropped audio at the start.

## Usage Modes

### 1. file Offline Mode (no ROS graph, runs CLI directly)

```bash
# Transcribe a PCM file (default model path ~/topsbot/models/whisper_small/engine_strip.json)
ros2 run topsbot_whisper_asr whisper_file_cli /path/to/input.pcm \
  --model-path ~/topsbot/models/whisper_small/engine_strip.json
```

Or via launch (internally calls `whisper_file_cli`):

```bash
ros2 launch topsbot_whisper_asr file_whisper.launch.py \
  pcm_file:=/path/to/input.pcm
```

### 2. Single-Node Online Mode (audio_raw → text)

```bash
ros2 launch topsbot_whisper_asr whisper.launch.py
# This mode subscribes to /audio_raw topic, waiting for other nodes to publish audio streams
# For example, use with a custom audio capture node
# Verify transcription output:
#   ros2 topic echo /asr_text
```

### 3. Joint Test Mode (audio source node + inference node, see scripts/audio_source_node.py)

```bash
ros2 launch topsbot_whisper_asr joint_test.launch.py \
  audio_file:=~/topsbot/models/whisper_small/output.wav
```

Real-time microphone input (see scripts/mic_source_node.py, uses `arecord` for capture):

```bash
ros2 launch topsbot_whisper_asr mic.launch.py
```

## Model Preparation

Models are downloaded from Alibaba Cloud Disk provided by `model-zoo/samples/Whisper`, 
with URLs and artifact manifest in `config/models_manifest.yaml`. Single-core int8 model 
(`models_zip_url`) is the default; dual-core combined mode uses `models_zip_url_2core` 
(available in `model-zoo`, commented out by default in this repo).

> The artifact entry point is the stripped **`engine_strip.json`** (see `model-zoo` README 
> section 3.1 manifest), which requires sibling files in the same directory: 
> `whisper_strip.alm`, `tokenizer.json`, `vocab.json`, `merges.txt`.

Download scripts (manifest-driven, parses Alibaba Cloud Disk share links):

```bash
bash scripts/download_models.sh          # Full download
bash scripts/ensure_models.sh            # Download only if missing
```

## Build (Cross-Compile)

Inside the `topsbot_cc` container:

```bash
topsbot_ws/tools/build.sh -s 'topsbot_whisper_asr'
```

On-board dependencies `libta_llm.so` / `libacuityllm_rt.so` are installed in `/usr/local/lib` 
with the sysroot.

## Testing

In-package gtest quick validation (pure logic, runs on PC/board):

```bash
colcon test --packages-select topsbot_whisper_asr
colcon test-result --verbose
```

## License

Apache License 2.0 — see [LICENSE](./LICENSE).

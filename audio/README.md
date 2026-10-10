# 测试音频

联合测试模式（`joint_test.launch.py`）默认从这里读取 `output.pcm`。该文件由
模型下载包提供（`config/models_manifest.yaml` 的 `optional_models` 里的
`output.pcm` / `output.wav`），也可自行放入任意 16 kHz 单声道 s16le PCM。

运行时可覆盖：

```bash
ros2 launch topsbot_whisper_asr joint_test.launch.py \
  audio_file:=/path/to/your/16k-mono.wav
```

`audio_source_node.py` 支持 `.wav`（16 kHz / 单声道 / 16-bit）与裸 s16le PCM。
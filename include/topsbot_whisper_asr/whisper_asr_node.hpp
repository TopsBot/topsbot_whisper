// Copyright 2026 TOPSBOT
// SPDX-License-Identifier: Apache-2.0

#ifndef TOPSBOT_WHISPER_ASR__WHISPER_ASR_NODE_HPP_
#define TOPSBOT_WHISPER_ASR__WHISPER_ASR_NODE_HPP_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/int8_multi_array.hpp"
#include "std_msgs/msg/string.hpp"

#include "topsbot_whisper_asr/whisper_audio.hpp"
#include "topsbot_whisper_asr/whisper_engine.hpp"

namespace topsbot_whisper_asr
{

/// Standalone ROS 2 node for Whisper ASR.
///
/// Subscribes to a raw-PCM audio topic (std_msgs/Int8MultiArray, s16le) and
/// accumulates audio into utterance-sized segments. Segmentation is VAD-based:
/// once speech has been detected (a chunk whose RMS clears
/// audio.vad_rms_threshold_), audio.idle_flush_seconds_ of trailing silence
/// ends the utterance (the audio.segment_seconds_ cap still applies for
/// continuous speech). Each segment is transcribed on a dedicated inference
/// thread and published as one std_msgs/String message. Whole-silent segments
/// are skipped without inference (audio.silence_rms_threshold_) and the backlog
/// is bounded when inference cannot keep up with real-time input
/// (audio.max_buffer_seconds_).
///
/// Parameters
/// ----------
/// input_topic          (string, default "/audio_raw")
/// text_topic           (string, default "/asr_text")
/// intent_topic         (string, default "/asr_intent")  nearest command word,
///                      empty to disable (only raw text is published).
/// model.path           (string, required) path to engine_strip.json
/// asr.task             (string, default "transcribe")  "transcribe"|"translate"
/// asr.language         (string, default "zh") ISO 639-1 code (zh/en/fr...) or
///                      full name (chinese/english/french...), empty = auto-detect
/// asr.prompt           (string, default "")  initial prompt
/// audio.sample_format  (string, default "s16le")  currently the only option
/// audio.sample_rate    (int,    default 16000)
/// audio.channels       (int,    default 1)
/// audio.segment_seconds       (double, default 30.0)  max continuous-speech window
/// audio.idle_flush_seconds    (double, default 1.0)   flush after this much trailing silence
/// audio.vad_rms_threshold     (double, default 0.010) a chunk >= this RMS counts as speech
/// audio.min_segment_seconds   (double, default 0.3)   never idle-flush a shorter partial
/// audio.silence_rms_threshold (double, default 0.005) skip all-silent segments; 0 = off
/// audio.max_buffer_seconds    (double, default 120.0) drop oldest audio past this; 0 = off
///
/// Segmentation policy (see inference_loop):
///   - A buffer reaching audio.segment_bytes_ is sliced as a 30 s "fixed-length"
///     segment and sent immediately (caps continuous speech).
///   - After VAD sees speech and audio.idle_flush_seconds_ of trailing silence
///     elapse, the buffer is sent as an "idle-flush" segment, BUT only once it
///     has reached 28/30 of a full segment. Sending a 0.4–2 s partial to
///     whisper_small (which has a fixed 30 s encode window with zero-pad for
///     shorter input) reliably produced training-set hallucinations on this
///     board, so we now hold partials until they're nearly full. This raises
///     end-to-end latency for short commands to ~30 s of trailing silence but
///     makes the recognition usable.
/// enable_profile       (bool,   default true)  log timing info
class WhisperAsrNode : public rclcpp::Node
{
public:
  explicit WhisperAsrNode(const rclcpp::NodeOptions & options = rclcpp::NodeOptions());
  ~WhisperAsrNode() override;

private:
  void initialize();

  void on_audio_raw(const std_msgs::msg::Int8MultiArray::ConstSharedPtr & msg);

  /// Dedicated inference thread: consume complete segments and idle flushes
  /// from audio_buffer_, transcribe them, and publish the text.
  void inference_loop();

  /// Convert an s16le byte buffer to double samples, transcribe, and publish
  /// the result. `first_sample` is the arrival time of this utterance's first
  /// audio byte (for end-to-end latency); `flush_time` is when the segment was
  /// handed to inference. Returns false only if the engine call failed.
  bool transcribe_and_publish(const std::vector<uint8_t> & bytes,
    double audio_seconds, bool is_flush,
    std::chrono::steady_clock::time_point first_sample,
    std::chrono::steady_clock::time_point flush_time);

  // ── Parameters ────────────────────────────────────────────────────────────
  std::string input_topic_{"/audio_raw"};
  std::string text_topic_{"/asr_text"};
  std::string intent_topic_{"/asr_intent"};
  std::string model_path_;
  std::string asr_task_{"transcribe"};
  std::string asr_language_;
  std::string asr_prompt_;
  std::string audio_sample_format_{"s16le"};
  int         audio_sample_rate_{16000};
  int         audio_channels_{1};
  double      audio_segment_seconds_{30.0};
  double      audio_idle_flush_seconds_{1.0};
  double      audio_vad_rms_threshold_{0.010};
  double      audio_min_segment_seconds_{0.3};
  double      audio_silence_rms_threshold_{0.005};
  double      audio_max_buffer_seconds_{120.0};
  bool        enable_profile_{true};

  // ── Derived ───────────────────────────────────────────────────────────────
  std::size_t audio_segment_bytes_{0U};
  std::size_t audio_max_buffer_bytes_{0U};   // 0 = unbounded
  std::size_t vad_chunk_bytes_{0U};          // one VAD analysis chunk
  std::size_t min_segment_bytes_{0U};        // min partial before idle flush
  double      bytes_per_second_{0.0};

  // ── Audio accumulation state (guarded by audio_mutex_) ────────────────────
  std::mutex                         audio_mutex_;
  std::condition_variable            audio_condition_;
  std::vector<uint8_t>               audio_buffer_;
  bool                               audio_have_speech_{false};  // heard speech this segment
  // Time of the last chunk whose RMS cleared the VAD threshold: idle-flush
  // fires when the newest chunk is below threshold and this point is farther
  // than audio_idle_flush_seconds_ in the past.
  std::chrono::steady_clock::time_point audio_last_speech_{};
  // First-sample arrival time of the utterance currently being accumulated, so
  // the end-to-end latency (mic -> published text) can be measured.
  std::chrono::steady_clock::time_point segment_first_sample_{};
  bool                               segment_have_first_sample_{false};

  // ── Inference thread ──────────────────────────────────────────────────────
  std::thread inference_thread_;
  std::atomic<bool> inference_running_{false};

  // ── ROS interfaces ────────────────────────────────────────────────────────
  rclcpp::Subscription<std_msgs::msg::Int8MultiArray>::SharedPtr audio_sub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr            text_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr            intent_pub_;
  rclcpp::TimerBase::SharedPtr                                   init_timer_;

  // ── Inference ─────────────────────────────────────────────────────────────
  WhisperEngine engine_;

  // ── Performance Statistics ────────────────────────────────────────────────
  struct PerformanceStats {
    size_t total_segments = 0;
    size_t successful_segments = 0;
    size_t failed_segments = 0;
    size_t skipped_silent = 0;
    size_t corrected_segments = 0;  // 纠错的段数
    size_t discarded_segments = 0;  // 丢弃的段数

    double total_audio_seconds = 0.0;
    double total_infer_ms = 0.0;
    double total_e2e_ms = 0.0;

    std::vector<float> rtf_samples;
    std::vector<float> e2e_samples;

    void add_success(double audio_s, float infer_ms, float e2e_ms, float rtf) {
      total_segments++;
      successful_segments++;
      total_audio_seconds += audio_s;
      total_infer_ms += infer_ms;
      total_e2e_ms += e2e_ms;
      rtf_samples.push_back(rtf);
      e2e_samples.push_back(e2e_ms);
    }

    void add_failure() {
      total_segments++;
      failed_segments++;
    }

    void add_skipped_silent() {
      skipped_silent++;
    }

    void add_corrected() {
      corrected_segments++;
    }

    void add_discarded() {
      discarded_segments++;
    }

    void print_summary(rclcpp::Logger logger) const;
  };

  PerformanceStats stats_;

  // ── Command word correction ───────────────────────────────────────────────
  /// List of valid command words for correction
  const std::vector<std::string> kCommandWords = {
    "前进", "后退", "左转", "右转", "停止",
    "加速", "减速", "开始", "结束", "暂停", "继续"
  };

  /// Correct recognized text to nearest command word
  /// Returns empty string if no good match found
  std::string correct_command(const std::string& text, float* confidence = nullptr) const;
};

}  // namespace topsbot_whisper_asr

#endif  // TOPSBOT_WHISPER_ASR__WHISPER_ASR_NODE_HPP_
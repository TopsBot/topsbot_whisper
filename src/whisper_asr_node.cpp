// Copyright 2026 TOPSBOT
// SPDX-License-Identifier: Apache-2.0

#include "topsbot_whisper_asr/whisper_asr_node.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "std_msgs/msg/string.hpp"

namespace topsbot_whisper_asr
{

namespace
{

template<typename T>
T read_param(rclcpp::Node & node, const std::string & name, const T & default_value)
{
  if (!node.has_parameter(name)) {
    node.declare_parameter(name, default_value);
  }
  return node.get_parameter(name).get_value<T>();
}

std::string lowercase(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(),
    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string expand_home(const std::string & path)
{
  if (path.empty() || path.front() != '~') {
    return path;
  }
  const char * home = std::getenv("HOME");
  if (!home || home[0] == '\0') {
    return path;
  }
  return std::string(home) + path.substr(1);
}

/// Calculate edit distance (Levenshtein distance) between two UTF-8 strings
int edit_distance(const std::string& s1, const std::string& s2)
{
  const size_t len1 = s1.size();
  const size_t len2 = s2.size();
  std::vector<std::vector<int>> d(len1 + 1, std::vector<int>(len2 + 1));

  for (size_t i = 0; i <= len1; ++i) d[i][0] = static_cast<int>(i);
  for (size_t j = 0; j <= len2; ++j) d[0][j] = static_cast<int>(j);

  for (size_t i = 1; i <= len1; ++i) {
    for (size_t j = 1; j <= len2; ++j) {
      int cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
      d[i][j] = std::min({
        d[i - 1][j] + 1,       // deletion
        d[i][j - 1] + 1,       // insertion
        d[i - 1][j - 1] + cost // substitution
      });
    }
  }
  return d[len1][len2];
}

/// Remove spaces and punctuation from string
std::string clean_string(const std::string& text)
{
  std::string cleaned;
  cleaned.reserve(text.size());
  for (unsigned char c : text) {
    if (!std::isspace(c) && !std::ispunct(c)) {
      cleaned.push_back(static_cast<char>(c));
    }
  }
  return cleaned;
}

}  // namespace

WhisperAsrNode::WhisperAsrNode(const rclcpp::NodeOptions & options)
: Node(
    "whisper_asr_node",
    rclcpp::NodeOptions(options).automatically_declare_parameters_from_overrides(true))
{
  // Defer heavy init (model load) off the constructor so the node is registered
  // with the executor before any blocking work starts.
  init_timer_ = create_wall_timer(
    std::chrono::milliseconds(1),
    [this]() {
      init_timer_->cancel();
      initialize();
    });
}

WhisperAsrNode::~WhisperAsrNode()
{
  // Print performance summary before shutdown
  if (enable_profile_) {
    stats_.print_summary(get_logger());
  }

  if (inference_thread_.joinable()) {
    inference_running_ = false;
    audio_condition_.notify_all();
    inference_thread_.join();
  }
}

void WhisperAsrNode::initialize()
{
  try {
    // ── Topic / model parameters ───────────────────────────────────────────
    input_topic_ = read_param(*this, "input_topic", std::string("/audio_raw"));
    text_topic_  = read_param(*this, "text_topic",  std::string("/asr_text"));
    intent_topic_ = read_param(*this, "intent_topic", std::string("/asr_intent"));
    model_path_  = expand_home(read_param(*this, "model.path", std::string("")));

    if (model_path_.empty() || !std::filesystem::is_regular_file(model_path_)) {
      throw std::invalid_argument(
        "model.path is not set or does not exist: " + model_path_);
    }

    // ── ASR options ────────────────────────────────────────────────────────
    asr_task_     = read_param(*this, "asr.task",     std::string("transcribe"));
    asr_language_ = read_param(*this, "asr.language", std::string("zh"));
    asr_prompt_   = read_param(*this, "asr.prompt",   std::string(""));

    if (!asr_task_.empty() && asr_task_ != "transcribe" && asr_task_ != "translate") {
      throw std::invalid_argument(
        "asr.task must be 'transcribe' or 'translate', got: " + asr_task_);
    }

    // ── Audio stream parameters ────────────────────────────────────────────
    audio_sample_format_ = lowercase(
      read_param(*this, "audio.sample_format", std::string("s16le")));
    audio_sample_rate_   = read_param(*this, "audio.sample_rate",        16000);
    audio_channels_      = read_param(*this, "audio.channels",           1);
    audio_segment_seconds_       = read_param(*this, "audio.segment_seconds",       30.0);
    audio_idle_flush_seconds_    = read_param(*this, "audio.idle_flush_seconds",    1.0);
    audio_vad_rms_threshold_     = read_param(*this, "audio.vad_rms_threshold",     0.010);
    audio_min_segment_seconds_   = read_param(*this, "audio.min_segment_seconds",   0.3);
    audio_silence_rms_threshold_ = read_param(*this, "audio.silence_rms_threshold", 0.005);
    audio_max_buffer_seconds_    = read_param(*this, "audio.max_buffer_seconds",    120.0);
    enable_profile_      = read_param(*this, "enable_profile", true);

    if (audio_sample_format_ != "s16le") {
      throw std::invalid_argument("audio.sample_format currently supports s16le only");
    }
    if (audio_sample_rate_ <= 0 || audio_channels_ <= 0) {
      throw std::invalid_argument("audio.sample_rate and audio.channels must be positive");
    }
    if (!(audio_segment_seconds_ > 0.0) || !(audio_idle_flush_seconds_ > 0.0)) {
      throw std::invalid_argument(
        "audio.segment_seconds and audio.idle_flush_seconds must be positive");
    }
    if (audio_vad_rms_threshold_ < 0.0 || audio_min_segment_seconds_ < 0.0 ||
      audio_silence_rms_threshold_ < 0.0 || audio_max_buffer_seconds_ < 0.0)
    {
      throw std::invalid_argument(
        "audio.vad_rms_threshold, audio.min_segment_seconds, "
        "audio.silence_rms_threshold and audio.max_buffer_seconds must be >= 0");
    }
    if (audio_min_segment_seconds_ >= audio_segment_seconds_) {
      throw std::invalid_argument(
        "audio.min_segment_seconds must be < audio.segment_seconds");
    }

    // 2 bytes per sample for s16le; align the segment down to whole frames.
    bytes_per_second_ =
      static_cast<double>(audio_sample_rate_) * audio_channels_ * 2.0;
    audio_segment_bytes_ = static_cast<std::size_t>(
      std::floor(audio_segment_seconds_ * bytes_per_second_));
    const std::size_t frame_bytes = static_cast<std::size_t>(audio_channels_) * 2U;
    audio_segment_bytes_ -= audio_segment_bytes_ % frame_bytes;
    if (audio_segment_bytes_ == 0U) {
      throw std::invalid_argument(
        "audio.segment_seconds is too short for even one sample frame");
    }

    // VAD granularity: analyse one ~100 ms chunk at a time.
    const std::size_t vad_chunk_samples =
      static_cast<std::size_t>(audio_sample_rate_ / 10U);
    vad_chunk_bytes_ = vad_chunk_samples * frame_bytes;
    if (vad_chunk_bytes_ == 0U) {
      vad_chunk_bytes_ = frame_bytes;
    }

    min_segment_bytes_ = static_cast<std::size_t>(
      std::floor(audio_min_segment_seconds_ * bytes_per_second_));
    min_segment_bytes_ -= min_segment_bytes_ % frame_bytes;

    // Backlog cap: keep at least one segment's worth of audio even if
    // max_buffer_seconds is misconfigured below it; 0 disables the cap.
    if (audio_max_buffer_seconds_ > 0.0) {
      audio_max_buffer_bytes_ = static_cast<std::size_t>(
        std::floor(audio_max_buffer_seconds_ * bytes_per_second_));
      audio_max_buffer_bytes_ -= audio_max_buffer_bytes_ % frame_bytes;
      audio_max_buffer_bytes_ = std::max(audio_max_buffer_bytes_, audio_segment_bytes_);
    }

    // ── Load model ─────────────────────────────────────────────────────────
    std::string engine_error;
    if (!engine_.load(model_path_, engine_error)) {
      throw std::runtime_error("Whisper engine load failed: " + engine_error);
    }

    // ── ROS interfaces ─────────────────────────────────────────────────────
    text_pub_ = create_publisher<std_msgs::msg::String>(text_topic_, 1);
    if (!intent_topic_.empty()) {
      intent_pub_ = create_publisher<std_msgs::msg::String>(intent_topic_, 1);
    }

    audio_sub_ = create_subscription<std_msgs::msg::Int8MultiArray>(
      input_topic_, 10,
      std::bind(&WhisperAsrNode::on_audio_raw, this, std::placeholders::_1));

    // ── Dedicated inference thread ─────────────────────────────────────────
    // Transcription runs off the executor so the subscription callback and the
    // solver's timers stay responsive while the NPU is busy.
    inference_running_ = true;
    inference_thread_ = std::thread(&WhisperAsrNode::inference_loop, this);

    RCLCPP_INFO(
      get_logger(),
      "whisper_asr_node ready: model=%s task=%s lang=%s "
      "audio=%s %dHz %dch segment=%.2fs (%zu bytes) idle_flush=%.2fs "
      "vad_rms>=%.4f min_segment=%.2fs silence_rms>=%.4f max_buffer=%.2fs "
      "input=%s text=%s intent=%s",
      model_path_.c_str(), asr_task_.c_str(),
      asr_language_.empty() ? "auto" : asr_language_.c_str(),
      audio_sample_format_.c_str(), audio_sample_rate_, audio_channels_,
      audio_segment_seconds_, audio_segment_bytes_, audio_idle_flush_seconds_,
      audio_vad_rms_threshold_, audio_min_segment_seconds_,
      audio_silence_rms_threshold_, audio_max_buffer_seconds_,
      input_topic_.c_str(), text_topic_.c_str(),
      intent_topic_.empty() ? "<disabled>" : intent_topic_.c_str());

  } catch (const std::exception & e) {
    RCLCPP_ERROR(get_logger(), "initialization failed: %s", e.what());
  }
}

void WhisperAsrNode::on_audio_raw(
  const std_msgs::msg::Int8MultiArray::ConstSharedPtr & msg)
{
  if (!engine_.is_loaded() || msg->data.empty()) {
    return;
  }

  // The message carries raw s16le bytes.  Check alignment.
  const std::size_t frame_bytes = static_cast<std::size_t>(audio_channels_) * 2U;
  const std::size_t data_bytes  = msg->data.size();
  const std::size_t usable      = data_bytes - (data_bytes % frame_bytes);

  if (usable == 0U) {
    RCLCPP_WARN_THROTTLE(
      get_logger(), *get_clock(), 2000,
      "received audio chunk smaller than one sample frame (%zu bytes), dropping",
      data_bytes);
    return;
  }
  if (usable < data_bytes) {
    RCLCPP_WARN_THROTTLE(
      get_logger(), *get_clock(), 2000,
      "audio chunk has %zu trailing sub-frame bytes; using only %zu bytes",
      data_bytes - usable, usable);
  }

  {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    const auto now = std::chrono::steady_clock::now();
    const auto * src = reinterpret_cast<const uint8_t *>(msg->data.data());
    // Record the first-sample arrival time of a new utterance: this is the
    // t=0 for the end-to-end latency metric (mic -> published text).
    if (!segment_have_first_sample_) {
      segment_first_sample_ = now;
      segment_have_first_sample_ = true;
    }
    audio_buffer_.insert(audio_buffer_.end(), src, src + usable);

    // ── VAD: classify the most recent ~100 ms chunk ─────────────────────────
    // Idle-flush is measured from the last *speech* chunk (not the last
    // *frame*), so a continuously-streaming mic can still end an utterance.
    const std::size_t chunk_bytes =
      std::min<std::size_t>(vad_chunk_bytes_, audio_buffer_.size());
    const std::size_t chunk_off = audio_buffer_.size() - chunk_bytes;
    const std::size_t chunk_samples = chunk_bytes / 2U;
    double chunk_energy = 0.0;
    for (std::size_t i = 0U; i < chunk_samples; ++i) {
      const int16_t sample = static_cast<int16_t>(
        static_cast<uint16_t>(audio_buffer_[chunk_off + i * 2U]) |
        (static_cast<uint16_t>(audio_buffer_[chunk_off + i * 2U + 1U]) << 8U));
      const double s = static_cast<double>(sample) / 32768.0;
      chunk_energy += s * s;
    }
    const double chunk_rms = chunk_samples > 0U
      ? std::sqrt(chunk_energy / static_cast<double>(chunk_samples)) : 0.0;
    if (chunk_rms >= audio_vad_rms_threshold_) {
      audio_have_speech_ = true;
      audio_last_speech_ = now;
    }

    // Inference is slower than real time on this NPU; if the backlog grows
    // past the cap, drop the oldest audio so latency stays bounded.
    if (audio_max_buffer_bytes_ > 0U && audio_buffer_.size() > audio_max_buffer_bytes_) {
      const std::size_t overflow = audio_buffer_.size() - audio_max_buffer_bytes_;
      audio_buffer_.erase(audio_buffer_.begin(),
        audio_buffer_.begin() + static_cast<std::ptrdiff_t>(overflow));
      RCLCPP_WARN_THROTTLE(
        get_logger(), *get_clock(), 5000,
        "audio backlog exceeded %.0fs cap; dropped %.2fs of oldest audio "
        "(inference cannot keep up with real-time input)",
        audio_max_buffer_seconds_,
        static_cast<double>(overflow) / bytes_per_second_);
    }
  }

  // Wake the inference thread: a full segment may now be ready, or a VAD
  // speech chunk just moved the idle-flush watermark (in which case it
  // re-waits and no-ops).
  audio_condition_.notify_all();
}

void WhisperAsrNode::inference_loop()
{
  const auto poll_interval = std::chrono::milliseconds(50);
  const auto idle_duration =
    std::chrono::duration<double>(audio_idle_flush_seconds_);

  while (inference_running_.load()) {
    std::vector<uint8_t> segment;
    double segment_seconds = 0.0;
    bool is_flush = false;
    std::chrono::steady_clock::time_point first_sample{};
    std::chrono::steady_clock::time_point flush_time{};

    {
      std::unique_lock<std::mutex> lock(audio_mutex_);
      audio_condition_.wait_for(lock, poll_interval, [this, &idle_duration]() {
        if (!inference_running_.load()) {
          return true;
        }
        if (audio_buffer_.size() >= audio_segment_bytes_) {
          return true;
        }
        if (!audio_have_speech_ || audio_buffer_.empty()) {
          return false;  // no speech yet, or nothing buffered
        }
        // Idle-flush is measured from the last *speech* chunk, not the last
        // *frame*, so a continuously-streaming mic can still end an utterance.
        const auto idle = std::chrono::steady_clock::now() - audio_last_speech_;
        return idle >= idle_duration;
      });

      if (!inference_running_.load()) {
        return;
      }

      if (audio_buffer_.size() >= audio_segment_bytes_) {
        // Complete fixed-length segment (continuous speech cap).
        segment.assign(
          audio_buffer_.begin(),
          audio_buffer_.begin() + static_cast<std::ptrdiff_t>(audio_segment_bytes_));
        audio_buffer_.erase(
          audio_buffer_.begin(),
          audio_buffer_.begin() + static_cast<std::ptrdiff_t>(audio_segment_bytes_));
        segment_seconds = audio_segment_seconds_;
        is_flush = false;
      } else if (audio_have_speech_ && audio_buffer_.size() >= min_segment_bytes_) {
        // Idle flush of a trailing utterance. Hold short buffers until they're
        // near a full segment: whisper_small uses a fixed 30 s encode window
        // (shorter inputs are zero-padded), and 28+ s of padding reliably
        // triggers Whisper hallucinations ("謝謝", "字幕君", ...). Cost is
        // ~segment_seconds_ of trailing-silence latency per command; that's
        // the right trade vs. gibberish output. kNearFullMarginSeconds keeps
        // us strictly below audio_segment_bytes_ so the 30 s cap branch above
        // still owns the "long continuous speech" path.
        constexpr double kNearFullMarginSeconds = 2.0;
        const double near_full_seconds =
          std::max(0.0, audio_segment_seconds_ - kNearFullMarginSeconds);
        const std::size_t near_full_bytes = static_cast<std::size_t>(
          std::floor(near_full_seconds * bytes_per_second_));
        // Align down to whole frames (same rule as initialize()).
        const std::size_t frame_bytes =
          static_cast<std::size_t>(audio_channels_) * 2U;
        const std::size_t aligned = std::max<std::size_t>(
          frame_bytes, near_full_bytes - (near_full_bytes % frame_bytes));
        if (audio_buffer_.size() < aligned) {
          // Not enough audio yet — wait for more (more speech, or for the
          // backlog cap to be hit so the user gets *something* within a bounded
          // delay). The wait_for predicate above will re-evaluate; nothing else
          // to do here.
          continue;
        }
        segment = std::move(audio_buffer_);
        audio_buffer_.clear();
        segment_seconds = static_cast<double>(segment.size()) / bytes_per_second_;
        is_flush = true;
      } else {
        continue;  // spurious notify / too short to flush
      }
      first_sample = segment_first_sample_;
      flush_time  = std::chrono::steady_clock::now();
      segment_have_first_sample_ = false;
      audio_have_speech_ = false;
    }

    transcribe_and_publish(
      segment, segment_seconds, is_flush, first_sample, flush_time);
  }
}

bool WhisperAsrNode::transcribe_and_publish(
  const std::vector<uint8_t> & bytes, double audio_seconds, bool is_flush,
  std::chrono::steady_clock::time_point first_sample,
  std::chrono::steady_clock::time_point flush_time)
{
  // Convert s16le → double (normalised to [-1, 1)).
  std::vector<double> samples;
  std::size_t sample_count = 0U;
  if (!pcm_s16le_to_double(bytes.data(), bytes.size(), samples, sample_count) ||
    sample_count == 0U)
  {
    RCLCPP_WARN(get_logger(), "failed to decode PCM segment, skipping");
    return false;
  }

  // Skip all-silent segments: each inference costs ~10s of NPU time on this
  // board, and Whisper hallucinates text (" m", " Thank you.", ...) on silence.
  if (audio_silence_rms_threshold_ > 0.0) {
    double energy = 0.0;
    for (const double s : samples) {
      energy += s * s;
    }
    const double rms = std::sqrt(energy / static_cast<double>(sample_count));
    if (rms < audio_silence_rms_threshold_) {
      // INFO (not DEBUG): short commands sit on mostly-silent buffers whose
      // whole-segment RMS dips below the gate, so logging is the only way to
      // see them being dropped.
      RCLCPP_INFO(get_logger(),
        "skipping %.2fs silent segment (rms=%.5f < %.4f)",
        audio_seconds, rms, audio_silence_rms_threshold_);
      stats_.add_skipped_silent();
      return false;
    }
  }

  const auto t0 = std::chrono::steady_clock::now();
  std::string text;
  std::string err;
  const bool ok = engine_.transcribe(samples, asr_task_, asr_language_, asr_prompt_,
    text, err);
  const auto t1 = std::chrono::steady_clock::now();

  if (!ok) {
    RCLCPP_WARN(get_logger(), "%s transcription failed: %s",
      is_flush ? "idle flush" : "segment", err.c_str());
    stats_.add_failure();
    return false;
  }

  // Don't publish empty / whitespace-only results.
  const auto first_nonspace = text.find_first_not_of(" \t\r\n");
  if (first_nonspace == std::string::npos) {
    RCLCPP_DEBUG(get_logger(), "%s: %.2fs audio produced no text",
      is_flush ? "idle-flush" : "transcribed", audio_seconds);
    return false;
  }
  const auto last_nonspace = text.find_last_not_of(" \t\r\n");
  text = text.substr(first_nonspace, last_nonspace - first_nonspace + 1U);

  // Filter hallucination: detect excessive character repetition (e.g., "君君君君...")
  // This is a common Whisper hallucination pattern on low-quality/silent audio.
  if (text.length() >= 6U) {
    // Check if the text consists mostly of repeated 2-3 character patterns
    bool is_repetitive = false;
    // Try 1-char repetition (e.g., "aaaaaa")
    if (text.length() >= 6U) {
      const char first_char = text[0];
      std::size_t same_count = 0U;
      for (char c : text) {
        if (c == first_char || c == ' ') { ++same_count; }
      }
      if (same_count >= text.length() * 0.8) {
        is_repetitive = true;
      }
    }
    // Try 2-char repetition (e.g., "君君君君君君")
    if (!is_repetitive && text.length() >= 8U) {
      const std::string pattern = text.substr(0, 2);
      std::size_t match_count = 0U;
      for (std::size_t i = 0U; i + 2U <= text.length(); i += 2U) {
        if (text.substr(i, 2) == pattern) { ++match_count; }
      }
      if (match_count >= text.length() / 2U * 0.8) {
        is_repetitive = true;
      }
    }
    // Try 3-char repetition (e.g., "字幕君字幕君字幕君")
    if (!is_repetitive && text.length() >= 12U) {
      const std::string pattern = text.substr(0, 3);
      std::size_t match_count = 0U;
      for (std::size_t i = 0U; i + 3U <= text.length(); i += 3U) {
        if (text.substr(i, 3) == pattern) { ++match_count; }
      }
      if (match_count >= text.length() / 3U * 0.8) {
        is_repetitive = true;
      }
    }

    if (is_repetitive) {
      RCLCPP_WARN(get_logger(),
        "skipping %.2fs audio with repetitive hallucination pattern: %s",
        audio_seconds, text.substr(0, 30).c_str());
      return false;
    }
  }

  if (enable_profile_) {
    const float infer_ms =
      std::chrono::duration<float, std::milli>(t1 - t0).count();
    const float rtf = (infer_ms / 1000.0f) / static_cast<float>(audio_seconds);

    // End-to-end latency: first audio byte of this utterance -> text published.
    if (first_sample.time_since_epoch().count() > 0) {
      const auto publish_time = std::chrono::steady_clock::now();
      const float e2e_ms = std::chrono::duration<float, std::milli>(
        publish_time - first_sample).count();
      const float buffer_ms = std::chrono::duration<float, std::milli>(
        flush_time - first_sample).count();

      // Record statistics
      stats_.add_success(audio_seconds, infer_ms, e2e_ms, rtf);

      RCLCPP_INFO(get_logger(),
        "%s: %.2fs audio | buffer_wait=%.0fms infer=%.0fms e2e=%.0fms RTF=%.2f | %s",
        is_flush ? "idle-flush" : "transcribed", audio_seconds,
        buffer_ms, infer_ms, e2e_ms, rtf, text.c_str());
    } else {
      // Record statistics (no e2e data)
      stats_.add_success(audio_seconds, infer_ms, 0.0f, rtf);

      RCLCPP_INFO(get_logger(), "%s: %.2fs audio in %.0f ms RTF=%.2f: %s",
        is_flush ? "idle-flush" : "transcribed", audio_seconds,
        infer_ms, rtf, text.c_str());
    }
  }

  std_msgs::msg::String out;
  out.data = text;
  text_pub_->publish(out);

  // Map the raw transcription to a discrete command if it lands near one of
  // the 11 known control words. Subscribers that only care about commands can
  // listen to intent_pub_ instead of sifting through free-form text. We keep
  // the raw text topic for downstream debugging and to avoid losing context
  // when the recogniser returns a sentence rather than a single word.
  if (intent_pub_) {
    float confidence = 0.0f;
    const std::string cmd = correct_command(text, &confidence);
    if (!cmd.empty() && confidence >= 0.3f) {
      std_msgs::msg::String intent_msg;
      // Format "<command>:<score>" so subscribers can threshold without
      // changing the message type. Stable prefix means downstream can also
      // split on ":" if it only wants the command word.
      intent_msg.data = cmd + ":" +
        std::to_string(static_cast<int>(std::lround(confidence * 100.0f)));
      intent_pub_->publish(intent_msg);
      RCLCPP_INFO(get_logger(),
        "intent: %s (raw=\"%s\", conf=%.2f)",
        intent_msg.data.c_str(), text.c_str(), confidence);
    }
  }

  return true;
}

void WhisperAsrNode::PerformanceStats::print_summary(rclcpp::Logger logger) const
{
  if (total_segments == 0) {
    return;
  }

  // Calculate percentiles
  auto rtf_sorted = rtf_samples;
  std::sort(rtf_sorted.begin(), rtf_sorted.end());
  auto e2e_sorted = e2e_samples;
  std::sort(e2e_sorted.begin(), e2e_sorted.end());

  const auto get_percentile = [](const std::vector<float>& v, float p) {
    if (v.empty()) return 0.0f;
    size_t idx = static_cast<size_t>(v.size() * p);
    return v[std::min(idx, v.size() - 1)];
  };

  RCLCPP_INFO(logger, "========== Performance Summary ==========");
  RCLCPP_INFO(logger, "Total segments: %zu (success=%zu, failed=%zu, skipped_silent=%zu)",
    total_segments, successful_segments, failed_segments, skipped_silent);
  RCLCPP_INFO(logger, "Total audio: %.2f s, Total infer: %.2f s",
    total_audio_seconds, total_infer_ms / 1000.0);

  if (total_audio_seconds > 0.0) {
    RCLCPP_INFO(logger, "Average RTF: %.2f",
      (total_infer_ms / 1000.0) / total_audio_seconds);
  }

  if (!rtf_sorted.empty()) {
    RCLCPP_INFO(logger, "RTF [P50=%.2f, P90=%.2f, P99=%.2f]",
      get_percentile(rtf_sorted, 0.5f),
      get_percentile(rtf_sorted, 0.9f),
      get_percentile(rtf_sorted, 0.99f));
  }

  if (!e2e_sorted.empty()) {
    RCLCPP_INFO(logger, "E2E latency [P50=%.0fms, P90=%.0fms, P99=%.0fms]",
      get_percentile(e2e_sorted, 0.5f),
      get_percentile(e2e_sorted, 0.9f),
      get_percentile(e2e_sorted, 0.99f));
  }

  if (total_segments > 0) {
    RCLCPP_INFO(logger, "Success rate: %.1f%%",
      100.0 * static_cast<double>(successful_segments) / static_cast<double>(total_segments));
  }

  if (corrected_segments > 0 || discarded_segments > 0) {
    RCLCPP_INFO(logger, "Corrected: %zu, Discarded: %zu", corrected_segments, discarded_segments);
  }

  RCLCPP_INFO(logger, "=========================================");
}

std::string WhisperAsrNode::correct_command(const std::string& text, float* confidence) const
{
  if (text.empty()) {
    return "";
  }

  // Clean input: remove spaces and punctuation
  std::string cleaned = clean_string(text);
  if (cleaned.empty()) {
    return "";
  }

  // Detect repetition (e.g., "右转右转右转右转...")
  // Check if any command word appears 3+ times consecutively
  for (const auto& cmd : kCommandWords) {
    size_t pos = 0;
    int count = 0;
    std::string search_text = cleaned;

    // Count occurrences
    while ((pos = search_text.find(cmd, pos)) != std::string::npos) {
      count++;
      pos += cmd.size();
    }

    // If repeated 3+ times, assume it's a loop and return single instance
    if (count >= 3) {
      if (confidence) *confidence = 0.8f;
      return cmd;
    }
  }

  // Find best match using edit distance
  int min_dist = INT_MAX;
  std::string best_match;

  for (const auto& cmd : kCommandWords) {
    // Exact match
    if (cleaned == cmd) {
      if (confidence) *confidence = 1.0f;
      return cmd;
    }

    // Substring match (contains)
    if (cleaned.find(cmd) != std::string::npos) {
      if (confidence) *confidence = 0.95f;
      return cmd;
    }

    // Command is contained in cleaned text
    if (cmd.find(cleaned) != std::string::npos) {
      if (confidence) *confidence = 0.9f;
      return cmd;
    }

    // Calculate edit distance
    int dist = edit_distance(cleaned, cmd);
    if (dist < min_dist) {
      min_dist = dist;
      best_match = cmd;
    }
  }

  // Calculate confidence based on edit distance
  size_t max_len = std::max(cleaned.size(), best_match.size());
  float conf = 1.0f - (static_cast<float>(min_dist) / static_cast<float>(max_len));

  if (confidence) *confidence = conf;

  // Threshold: only accept if confidence > 0.3 (lowered for better recall)
  if (conf > 0.3f) {
    return best_match;
  }

  return "";  // No good match
}

}  // namespace topsbot_whisper_asr

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<topsbot_whisper_asr::WhisperAsrNode>();
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}
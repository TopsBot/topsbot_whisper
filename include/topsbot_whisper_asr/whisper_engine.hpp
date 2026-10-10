// Copyright 2026 TOPSBOT
// SPDX-License-Identifier: Apache-2.0

#ifndef TOPSBOT_WHISPER_ASR__WHISPER_ENGINE_HPP_
#define TOPSBOT_WHISPER_ASR__WHISPER_ENGINE_HPP_

#include <string>
#include <vector>

#include "ta_llm.h"

namespace topsbot_whisper_asr
{

/// RAII wrapper around a ta-llmruntime ASR context.
///
/// Lifecycle: ta_llm_create(engine.json, "viplite", ASR) -> ta_llm_asr_run.
/// Each instance is intended to belong to a single worker thread, so the class
/// is not internally synchronized.
class WhisperEngine
{
public:
  WhisperEngine() = default;
  ~WhisperEngine();

  WhisperEngine(const WhisperEngine &) = delete;
  WhisperEngine & operator=(const WhisperEngine &) = delete;
  WhisperEngine(WhisperEngine && other) noexcept;
  WhisperEngine & operator=(WhisperEngine && other) noexcept;

  /// Load and prepare the engine described by `engine_json_path`. Returns true
  /// on success and leaves `error` populated on failure. The previous engine, if
  /// any, is released first.
  bool load(const std::string & engine_json_path, std::string & error);

  /// Run ASR over normalized audio ([-1, 1), see pcm_s16le_to_double). On
  /// success `text` receives the UTF-8 transcription.
  ///
  /// @param task "transcribe" (default) or "translate"
  /// @param language ISO 639-1 language code (e.g., "zh", "en", "fr") or full
  ///                 language name (e.g., "chinese", "english", "french").
  ///                 Empty string enables auto-detection.
  ///                 Note: The implementation automatically maps ISO codes to
  ///                 full names as required by the underlying SDK.
  /// @param prompt Reserved for future use (currently unused by ta-llmruntime)
  bool transcribe(
    const std::vector<double> & audio, const std::string & task,
    const std::string & language, const std::string & prompt, std::string & text,
    std::string & error);

  bool is_loaded() const noexcept {return ctx_ != nullptr;}

private:
  void release() noexcept;

  ta_llm_ctx ctx_{nullptr};
};

}  // namespace topsbot_whisper_asr

#endif  // TOPSBOT_WHISPER_ASR__WHISPER_ENGINE_HPP_

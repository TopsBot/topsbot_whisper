// Copyright 2026 TOPSBOT
// SPDX-License-Identifier: Apache-2.0

#include "topsbot_whisper_asr/whisper_engine.hpp"

#include <unordered_map>
#include <utility>

#include "ta_llm.h"

namespace topsbot_whisper_asr
{

namespace {

constexpr const char * kBackendName = "viplite";

/// Map ISO 639-1 language codes to full language names expected by the SDK.
/// The SDK's underlying Whisper tokenizer expects full names ("chinese"),
/// not ISO codes ("zh"), but the ta_llm.h API documented as accepting
/// "language code". This mapping allows both forms to work.
const char * iso_to_language_name(const char * lang)
{
  if (lang == nullptr || lang[0] == '\0') {
    return nullptr;  // empty/null -> auto-detect
  }

  static const std::unordered_map<std::string, const char *> kLangMap = {
    // ISO code -> full name (primary mapping)
    {"zh", "chinese"},   {"en", "english"},    {"de", "german"},
    {"es", "spanish"},   {"ru", "russian"},    {"ko", "korean"},
    {"fr", "french"},    {"ja", "japanese"},   {"pt", "portuguese"},
    {"tr", "turkish"},   {"pl", "polish"},     {"ca", "catalan"},
    {"nl", "dutch"},     {"ar", "arabic"},     {"sv", "swedish"},
    {"it", "italian"},   {"id", "indonesian"}, {"hi", "hindi"},
    {"fi", "finnish"},   {"vi", "vietnamese"}, {"he", "hebrew"},
    {"uk", "ukrainian"}, {"el", "greek"},      {"ms", "malay"},
    {"cs", "czech"},     {"ro", "romanian"},   {"da", "danish"},
    {"hu", "hungarian"}, {"ta", "tamil"},      {"no", "norwegian"},
    {"th", "thai"},      {"ur", "urdu"},       {"hr", "croatian"},
    {"bg", "bulgarian"}, {"lt", "lithuanian"}, {"la", "latin"},

    // Full name -> full name (passthrough for backward compatibility)
    {"chinese", "chinese"},     {"english", "english"},
    {"german", "german"},       {"spanish", "spanish"},
    {"russian", "russian"},     {"korean", "korean"},
    {"french", "french"},       {"japanese", "japanese"},
    {"portuguese", "portuguese"}, {"turkish", "turkish"},
    {"polish", "polish"},       {"catalan", "catalan"},
    {"dutch", "dutch"},         {"arabic", "arabic"},
    {"swedish", "swedish"},     {"italian", "italian"},
    {"indonesian", "indonesian"}, {"hindi", "hindi"},
    {"finnish", "finnish"},     {"vietnamese", "vietnamese"},
    {"hebrew", "hebrew"},       {"ukrainian", "ukrainian"},
    {"greek", "greek"},         {"malay", "malay"},
    {"czech", "czech"},         {"romanian", "romanian"},
    {"danish", "danish"},       {"hungarian", "hungarian"},
    {"tamil", "tamil"},         {"norwegian", "norwegian"},
    {"thai", "thai"},           {"urdu", "urdu"},
    {"croatian", "croatian"},   {"bulgarian", "bulgarian"},
    {"lithuanian", "lithuanian"}, {"latin", "latin"},
  };

  auto it = kLangMap.find(lang);
  // If found in map, return the full name; otherwise return input as-is
  // (allows SDK to handle unknown languages or future additions)
  return it != kLangMap.end() ? it->second : lang;
}

/// State gathered by the streaming callback as the ASR engine emits text.
struct AsrStreamCapture
{
  std::string accumulated;  // concatenation of every fragment received
};

/// ta-llm stream callback: invoked for each emitted text fragment.
/// Return 0 to continue, non-zero to request abort.
int asr_stream_cb(void * user_data, const char * utf8_text)
{
  if (user_data == nullptr || utf8_text == nullptr) {
    return 0;
  }
  auto * capture = static_cast<AsrStreamCapture *>(user_data);
  capture->accumulated += utf8_text;
  return 0;
}

}  // namespace

WhisperEngine::~WhisperEngine()
{
  release();
}

WhisperEngine::WhisperEngine(WhisperEngine && other) noexcept
: ctx_(other.ctx_)
{
  other.ctx_ = nullptr;
}

WhisperEngine & WhisperEngine::operator=(WhisperEngine && other) noexcept
{
  if (this != &other) {
    release();
    ctx_ = other.ctx_;
    other.ctx_ = nullptr;
  }
  return *this;
}

void WhisperEngine::release() noexcept
{
  if (ctx_ != nullptr) {
    ta_llm_destroy(ctx_);
    ctx_ = nullptr;
  }
}

bool WhisperEngine::load(const std::string & engine_json_path, std::string & error)
{
  release();
  ctx_ = ta_llm_create(
    engine_json_path.c_str(), kBackendName, ACUITY_LLM_ENGINE_TYPE_ASR);
  if (ctx_ == nullptr) {
    error = "ta_llm_create failed for: " + engine_json_path;
    return false;
  }
  error.clear();
  return true;
}

bool WhisperEngine::transcribe(
  const std::vector<double> & audio, const std::string & task,
  const std::string & language, const std::string & /*prompt*/, std::string & text,
  std::string & error)
{
  text.clear();
  if (ctx_ == nullptr) {
    error = "whisper engine is not loaded";
    return false;
  }
  if (audio.empty()) {
    error = "audio buffer is empty";
    return false;
  }

  const char * task_str     = task.empty()     ? nullptr : task.c_str();
  const char * language_str = language.empty() ? nullptr : iso_to_language_name(language.c_str());
  const char * response     = nullptr;

  // The SDK's non-streaming `generate` returns a growing, prefix-accumulated
  // buffer for ASR, not a final transcription.  Use the streaming entry point
  // instead so we can reconstruct clean text from the emitted fragments.
  AsrStreamCapture capture;

  const int ret = ta_llm_asr_run_stream(
    ctx_,
    audio.data(),
    static_cast<int>(audio.size()),
    task_str,
    language_str,
    asr_stream_cb,
    &capture,
    &response);

  if (ret != 0) {
    error = "ta_llm_asr_run_stream failed (ret=" + std::to_string(ret) + ")";
    return false;
  }

  if (capture.accumulated.empty()) {
    error = "ta_llm_asr_run_stream produced no text";
    return false;
  }

  text = capture.accumulated;
  error.clear();
  return true;
}

}  // namespace topsbot_whisper_asr

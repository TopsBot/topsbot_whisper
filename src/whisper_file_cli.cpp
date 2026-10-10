// Copyright 2026 TOPSBOT
// SPDX-License-Identifier: Apache-2.0

// Offline ASR: transcribe a single s16le PCM file through WhisperEngine.
// Mirrors `acuity-llm-asr-cli engine.json output.pcm`, but reuses the exact
// engine wrapper the standalone ROS node uses.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "topsbot_whisper_asr/whisper_audio.hpp"
#include "topsbot_whisper_asr/whisper_engine.hpp"

namespace
{

void print_usage(const char * executable)
{
  std::cerr
    << "Usage: " << executable << " <input.pcm>\n"
    << "  --model-path PATH   engine_strip.json (default: ~/topsbot/models/whisper_small/engine_strip.json)\n"
    << "  --task TYPE         transcribe|translate (default: transcribe)\n"
    << "  --language CODE     2-letter code (default: auto-detect)\n"
    << "  --prompt TEXT       initial prompt (default: empty)\n";
}

std::string next_argument(int argc, char ** argv, int & index)
{
  if (index + 1 >= argc) {
    return {};
  }
  return argv[++index];
}

std::string expand_user_path(const std::string & path)
{
  if (path.empty() || path.front() != '~') {
    return path;
  }
  const char * user_home = std::getenv("HOME");
  if (!user_home || user_home[0] == '\0') {
    return path;
  }
  if (path.size() == 1U || path[1] == '/') {
    return std::string(user_home) + path.substr(1);
  }
  return path;
}

}  // namespace

int main(int argc, char ** argv)
{
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  const std::string input_path = expand_user_path(argv[1]);
  std::string model_path = "~/topsbot/models/whisper_small/engine_strip.json";
  std::string task = "transcribe";
  std::string language;
  std::string prompt;

  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--model-path") {
      model_path = next_argument(argc, argv, index);
    } else if (argument == "--task") {
      task = next_argument(argc, argv, index);
    } else if (argument == "--language") {
      language = next_argument(argc, argv, index);
    } else if (argument == "--prompt") {
      prompt = next_argument(argc, argv, index);
    } else {
      std::cerr << "unknown option: " << argument << '\n';
      print_usage(argv[0]);
      return 1;
    }
  }

  if (!std::filesystem::is_regular_file(input_path)) {
    std::cerr << "error: input file not found: " << input_path << '\n';
    return 1;
  }

  std::vector<double> samples;
  if (topsbot_whisper_asr::load_pcm_file_to_double(input_path, samples) == 0U) {
    std::cerr << "error: failed to read or decode input PCM: " << input_path << '\n';
    return 1;
  }

  model_path = expand_user_path(model_path);
  topsbot_whisper_asr::WhisperEngine engine;
  std::string error;
  if (!engine.load(model_path, error)) {
    std::cerr << "error: whisper initialization failed: " << error << '\n';
    return 1;
  }

  std::string text;
  const auto infer_begin = std::chrono::steady_clock::now();
  const bool ok = engine.transcribe(samples, task, language, prompt, text, error);
  const auto infer_end = std::chrono::steady_clock::now();

  if (!ok) {
    std::cerr << "error: whisper inference failed: " << error << '\n';
    return 1;
  }

  std::cout << text << '\n';
  std::cerr << "infer_ms="
            << std::chrono::duration<float, std::milli>(infer_end - infer_begin).count()
            << '\n';
  return 0;
}
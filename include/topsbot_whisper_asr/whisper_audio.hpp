// Copyright 2026 TOPSBOT
// SPDX-License-Identifier: Apache-2.0

#ifndef TOPSBOT_WHISPER_ASR__WHISPER_AUDIO_HPP_
#define TOPSBOT_WHISPER_ASR__WHISPER_AUDIO_HPP_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace topsbot_whisper_asr
{

/// Convert interleaved s16le PCM samples to normalized doubles in [-1, 1),
/// dividing by 32768 as Whisper expects. A trailing odd byte is ignored;
/// `sample_count` receives the number of whole 16-bit samples produced.
///
/// Returns true when at least one whole sample could be decoded. Callers that
/// care distinguish "no samples" (empty input) from a hard failure via
/// `sample_count` being zero.
bool pcm_s16le_to_double(
  const uint8_t * data, std::size_t byte_count, std::vector<double> & out,
  std::size_t & sample_count) noexcept;

/// Read an entire file into a byte buffer. Returns false when the file cannot
/// be opened.
bool read_binary_file(const std::string & path, std::vector<uint8_t> & out);

/// Read an s16le PCM file and convert it to normalized doubles, returning the
/// number of samples. Returns 0 (and leaves `out` empty) on any failure.
std::size_t load_pcm_file_to_double(const std::string & path, std::vector<double> & out);

}  // namespace topsbot_whisper_asr

#endif  // TOPSBOT_WHISPER_ASR__WHISPER_AUDIO_HPP_
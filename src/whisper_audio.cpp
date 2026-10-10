// Copyright 2026 TOPSBOT
// SPDX-License-Identifier: Apache-2.0

#include "topsbot_whisper_asr/whisper_audio.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>

namespace topsbot_whisper_asr
{

bool pcm_s16le_to_double(
  const uint8_t * data, std::size_t byte_count, std::vector<double> & out,
  std::size_t & sample_count) noexcept
{
  out.clear();
  sample_count = 0U;
  if (data == nullptr || byte_count < 2U) {
    return false;
  }
  const std::size_t count = byte_count / 2U;
  out.reserve(count);
  for (std::size_t index = 0U; index < count; ++index) {
    // Little-endian s16: low byte first, then sign-extended high byte.
    const int16_t sample = static_cast<int16_t>(
      static_cast<uint16_t>(data[index * 2U]) |
      (static_cast<uint16_t>(data[index * 2U + 1U]) << 8U));
    out.push_back(static_cast<double>(sample) / 32768.0);
  }
  sample_count = count;
  return true;
}

bool read_binary_file(const std::string & path, std::vector<uint8_t> & out)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    out.clear();
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return true;
}

std::size_t load_pcm_file_to_double(const std::string & path, std::vector<double> & out)
{
  std::vector<uint8_t> bytes;
  if (!read_binary_file(path, bytes)) {
    out.clear();
    return 0U;
  }
  std::size_t count = 0U;
  if (!pcm_s16le_to_double(bytes.data(), bytes.size(), out, count)) {
    out.clear();
    return 0U;
  }
  return count;
}

}  // namespace topsbot_whisper_asr
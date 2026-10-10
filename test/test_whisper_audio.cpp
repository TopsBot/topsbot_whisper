// Copyright 2026 TOPSBOT
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "topsbot_whisper_asr/whisper_audio.hpp"

namespace
{

std::vector<uint8_t> s16le_bytes(const std::vector<int16_t> & samples)
{
  std::vector<uint8_t> bytes;
  bytes.reserve(samples.size() * 2U);
  for (const int16_t sample : samples) {
    bytes.push_back(static_cast<uint8_t>(sample & 0xFF));
    bytes.push_back(static_cast<uint8_t>((sample >> 8) & 0xFF));
  }
  return bytes;
}

}  // namespace

TEST(WhisperAudio, ConvertsS16leToNormalizedDouble)
{
  const std::vector<int16_t> raw{0, 16384, -16384, 32767, -32768};
  const auto bytes = s16le_bytes(raw);

  std::vector<double> out;
  std::size_t count = 0U;
  ASSERT_TRUE(topsbot_whisper_asr::pcm_s16le_to_double(bytes.data(), bytes.size(), out, count));
  ASSERT_EQ(count, raw.size());
  ASSERT_EQ(out.size(), raw.size());

  EXPECT_NEAR(out[0], 0.0, 1e-12);
  EXPECT_NEAR(out[1], 0.5, 1e-12);
  EXPECT_NEAR(out[2], -0.5, 1e-12);
  EXPECT_NEAR(out[3], 32767.0 / 32768.0, 1e-12);
  EXPECT_NEAR(out[4], -1.0, 1e-12);
}

TEST(WhisperAudio, IgnoresTrailingOddByte)
{
  const auto bytes = s16le_bytes({1000});
  std::vector<uint8_t> padded = bytes;
  padded.push_back(0xFF);  // dangling low byte of a would-be second sample

  std::vector<double> out;
  std::size_t count = 0U;
  ASSERT_TRUE(topsbot_whisper_asr::pcm_s16le_to_double(padded.data(), padded.size(), out, count));
  ASSERT_EQ(count, 1U);
  EXPECT_NEAR(out[0], 1000.0 / 32768.0, 1e-12);
}

TEST(WhisperAudio, RejectsEmptyAndTruncatedInput)
{
  std::vector<double> out;
  std::size_t count = 42U;  // must be reset by the routine
  EXPECT_FALSE(topsbot_whisper_asr::pcm_s16le_to_double(nullptr, 0U, out, count));
  EXPECT_EQ(count, 0U);

  const uint8_t single = 0x01;
  EXPECT_FALSE(topsbot_whisper_asr::pcm_s16le_to_double(&single, 1U, out, count));
  EXPECT_EQ(count, 0U);
  EXPECT_TRUE(out.empty());
}
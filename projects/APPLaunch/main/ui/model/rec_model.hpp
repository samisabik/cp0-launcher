/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Pure helpers for the Rec page (recorder with on-device speech to text),
// kept free of LVGL and process access so they can be unit-tested on the host.
namespace rec {

// "20261115_044536", the file name stem for a recording.
std::string recording_stem(int year, int month, int day, int hour, int minute, int second);

// "11-15 04:45" for a recording stem; other stems are returned unchanged.
std::string stem_label(const std::string &stem);

// "0:07", "1:05", "12:00".
std::string format_duration(uint32_t ms);

// Length of a RIFF/WAVE file from its first bytes and total size. Handles PCM
// and float data, and data chunks whose size field is still a placeholder
// while the file is being written. Returns 0 for anything else.
uint32_t wav_duration_ms(const uint8_t *header, std::size_t header_bytes, uint64_t file_size);

// whisper-cli -otxt output to one paragraph: segments joined with single
// spaces, non-speech annotations such as [BLANK_AUDIO] or (wind) removed.
std::string clean_transcript(const std::string &raw);

// Text cut to max_chars code points.
std::string clip_text(const std::string &text, std::size_t max_chars);

// Peak of 16-bit little-endian PCM as 0..100 on a -60..0 dBFS scale.
int peak_level(const uint8_t *pcm, std::size_t bytes);

// Expected transcription time: conversion and model load, then about 1.25x
// the clip length for whisper tiny.en on the CM0.
uint32_t transcribe_estimate_ms(uint32_t duration_ms);

} // namespace rec

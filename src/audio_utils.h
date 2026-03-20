#pragma once

#include "common.h"

class RingBuffer {
 public:
  explicit RingBuffer(size_t capacity) : buf_(capacity) {}

  size_t capacity() const { return buf_.size(); }
  size_t size() const { return size_; }
  size_t free() const { return buf_.size() - size_; }

  size_t Write(const uint8_t* data, size_t bytes) {
    if (bytes == 0 || bytes > free()) {
      return 0;
    }
    const size_t first = std::min(bytes, buf_.size() - head_);
    std::memcpy(buf_.data() + head_, data, first);
    const size_t remaining = bytes - first;
    if (remaining > 0) {
      std::memcpy(buf_.data(), data + first, remaining);
    }
    head_ = (head_ + bytes) % buf_.size();
    size_ += bytes;
    return bytes;
  }

  size_t Read(uint8_t* out, size_t bytes) {
    if (bytes == 0 || size_ == 0) {
      return 0;
    }
    const size_t to_read = std::min(bytes, size_);
    const size_t first = std::min(to_read, buf_.size() - tail_);
    std::memcpy(out, buf_.data() + tail_, first);
    const size_t remaining = to_read - first;
    if (remaining > 0) {
      std::memcpy(out + first, buf_.data(), remaining);
    }
    tail_ = (tail_ + to_read) % buf_.size();
    size_ -= to_read;
    return to_read;
  }

  void Clear() {
    head_ = 0;
    tail_ = 0;
    size_ = 0;
  }

 private:
  std::vector<uint8_t> buf_;
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t size_ = 0;
};

std::string FormatHms(uint64_t total_sec) {
  const uint64_t hh = total_sec / 3600;
  const uint64_t mm = (total_sec % 3600) / 60;
  const uint64_t ss = total_sec % 60;
  char out[32];
  std::snprintf(out, sizeof(out), "%02llu:%02llu:%02llu",
                static_cast<unsigned long long>(hh),
                static_cast<unsigned long long>(mm),
                static_cast<unsigned long long>(ss));
  return std::string(out);
}

int ComputePeakPercent(const uint8_t* data, size_t bytes, snd_pcm_format_t fmt) {
  if (!data || bytes == 0) {
    return 0;
  }

  if (fmt == SND_PCM_FORMAT_S16_LE) {
    const size_t samples = bytes / 2;
    int32_t max_abs = 0;
    for (size_t i = 0; i < samples; ++i) {
      const size_t o = i * 2;
      const int16_t s = static_cast<int16_t>(
          static_cast<uint16_t>(data[o]) |
          (static_cast<uint16_t>(data[o + 1]) << 8));
      const int32_t a = std::abs(static_cast<int32_t>(s));
      if (a > max_abs) {
        max_abs = a;
      }
    }
    return static_cast<int>((max_abs * 100LL) / 32767LL);
  }

  if (fmt == SND_PCM_FORMAT_S24_3LE) {
    const size_t samples = bytes / 3;
    int32_t max_abs = 0;
    for (size_t i = 0; i < samples; ++i) {
      const size_t o = i * 3;
      int32_t v = static_cast<int32_t>(data[o]) |
                  (static_cast<int32_t>(data[o + 1]) << 8) |
                  (static_cast<int32_t>(data[o + 2]) << 16);
      if (v & 0x00800000) {
        v |= ~0x00FFFFFF;
      }
      const int32_t a = std::abs(v);
      if (a > max_abs) {
        max_abs = a;
      }
    }
    return static_cast<int>((max_abs * 100LL) / 8388607LL);
  }

  return 0;
}

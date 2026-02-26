#include <alsa/asoundlib.h>
#include <sndfile.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <gpiod.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {

enum class MicKind {
  kUnspecified,
  kSpcmic,
  kZylia,
};

struct Options {
  std::string device = "default";
  std::string out_path;
  int rate = 48000;
  int channels = 84;
  MicKind mic = MicKind::kUnspecified;
  snd_pcm_format_t format = SND_PCM_FORMAT_S24_3LE;
  snd_pcm_access_t access = SND_PCM_ACCESS_RW_INTERLEAVED;
  bool out_overridden = false;
  bool device_overridden = false;
  bool channels_overridden = false;
  bool access_overridden = false;
  bool explicit_start = true;
  bool stdin_raw = false;
  bool hat_ui = false;
  int buffer_ms = 200;
  int period_ms = 50;
  int ring_ms = 5000;
  int status_ms = 0;
  bool list_devices = false;
  bool show_help = false;
};

std::atomic<bool> g_running{true};

void HandleSignal(int) {
  g_running.store(false);
}

const char* MicKindToString(MicKind mic) {
  if (mic == MicKind::kSpcmic) {
    return "spcmic";
  }
  if (mic == MicKind::kZylia) {
    return "zylia";
  }
  return "";
}

std::string BuildAutoOutPath(MicKind mic) {
  const char* mic_name = MicKindToString(mic);
  std::time_t now = std::time(nullptr);
  std::tm tm_now{};
#ifdef _WIN32
  localtime_s(&tm_now, &now);
#else
  localtime_r(&now, &tm_now);
#endif
  std::ostringstream oss;
  oss << mic_name << "_" << std::put_time(&tm_now, "%Y%m%d_%H%M%S") << ".rf64";
  return oss.str();
}

std::string BuildManualTakePath(const std::string& base_path, int take_index) {
  if (take_index <= 1) {
    return base_path;
  }
  const size_t slash = base_path.find_last_of("/\\");
  const size_t dot = base_path.find_last_of('.');
  const bool has_ext = (dot != std::string::npos) &&
                       (slash == std::string::npos || dot > slash);
  const std::string stem = has_ext ? base_path.substr(0, dot) : base_path;
  const std::string ext = has_ext ? base_path.substr(dot) : "";
  char suffix[32];
  std::snprintf(suffix, sizeof(suffix), "_take%03d", take_index);
  return stem + suffix + ext;
}

void PrintUsage(const char* exe) {
  std::printf(
      "Usage: %s [options]\n"
      "\n"
      "Minimal multichannel recorder using ALSA + RF64 WAV.\n"
      "\n"
      "Options:\n"
      "  -d, --device <name>     ALSA device (default: \"default\")\n"
      "  -o, --out <path>        Output RF64 file (default: auto name)\n"
      "  -r, --rate <48000|96000> Sample rate (default: 48000)\n"
      "  -c, --channels <n>      Channel count (default: 84)\n"
      "  --mic <spcmic|zylia>    Mic preset for device/channels/access\n"
      "  -f, --format <s16|s24>  Sample format (default: s24)\n"
      "  --access <rw|mmap>      ALSA access type (default: rw)\n"
      "  --start <auto|explicit> Stream start mode (default: explicit)\n"
      "  --stdin-raw             Read raw PCM from stdin instead of ALSA\n"
      "  --hat-ui                Enable Waveshare 1.3inch LCD HAT status UI\n"
      "  --buffer-ms <ms>        ALSA buffer time in ms (default: 200)\n"
      "  --period-ms <ms>        ALSA period time in ms (default: 50)\n"
      "  --ring-ms <ms>          Ring buffer time in ms (default: 5000)\n"
      "  --status-ms <ms>        Print status every N ms (default: 0=off)\n"
      "  -L, --list-devices      List ALSA PCM devices and exit\n"
      "  -h, --help              Show this help\n"
      "\n"
      "Mic presets:\n"
      "  spcmic => device=hw:CARD=s02E5D5,DEV=0 channels=84 access=rw\n"
      "  zylia  => device=hw:CARD=ZM13E,DEV=0   channels=19 access=mmap\n"
      "  Explicit --device/--channels/--access override preset values.\n"
      "\n"
      "Auto naming:\n"
      "  If --out is omitted: <mic>_YYYYMMDD_HHMMSS.rf64\n"
      "  Auto naming requires --mic spcmic|zylia.\n"
      "\n"
      "Waveshare HAT controls:\n"
      "  With --hat-ui the app starts in IDLE and waits for KEY2.\n"
      "  KEY1 = stop take (back to IDLE) | KEY2 = start recording\n"
      "  KEY3 = backlight toggle\n",
      exe);
}

bool ParseArgs(int argc, char** argv, Options* out) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto need_value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Missing value for %s\n", name);
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "-h" || arg == "--help") {
      out->show_help = true;
      return true;
    }
    if (arg == "-L" || arg == "--list-devices") {
      out->list_devices = true;
      continue;
    }
    if (arg == "-d" || arg == "--device") {
      const char* v = need_value(arg.c_str());
      if (!v) return false;
      out->device = v;
      out->device_overridden = true;
      continue;
    }
    if (arg == "-o" || arg == "--out") {
      const char* v = need_value(arg.c_str());
      if (!v) return false;
      out->out_path = v;
      out->out_overridden = true;
      continue;
    }
    if (arg == "-r" || arg == "--rate") {
      const char* v = need_value(arg.c_str());
      if (!v) return false;
      out->rate = std::atoi(v);
      continue;
    }
    if (arg == "-c" || arg == "--channels") {
      const char* v = need_value(arg.c_str());
      if (!v) return false;
      out->channels = std::atoi(v);
      out->channels_overridden = true;
      continue;
    }
    if (arg == "--mic") {
      const char* v = need_value(arg.c_str());
      if (!v) return false;
      if (std::strcmp(v, "spcmic") == 0) {
        out->mic = MicKind::kSpcmic;
      } else if (std::strcmp(v, "zylia") == 0) {
        out->mic = MicKind::kZylia;
      } else {
        std::fprintf(stderr, "Unknown mic: %s (use spcmic or zylia)\n", v);
        return false;
      }
      continue;
    }
    if (arg == "-f" || arg == "--format") {
      const char* v = need_value(arg.c_str());
      if (!v) return false;
      if (std::strcmp(v, "s16") == 0) {
        out->format = SND_PCM_FORMAT_S16_LE;
      } else if (std::strcmp(v, "s24") == 0) {
        out->format = SND_PCM_FORMAT_S24_3LE;
      } else {
        std::fprintf(stderr, "Unknown format: %s (use s16 or s24)\n", v);
        return false;
      }
      continue;
    }
    if (arg == "--access") {
      const char* v = need_value(arg.c_str());
      if (!v) return false;
      if (std::strcmp(v, "rw") == 0) {
        out->access = SND_PCM_ACCESS_RW_INTERLEAVED;
      } else if (std::strcmp(v, "mmap") == 0) {
        out->access = SND_PCM_ACCESS_MMAP_INTERLEAVED;
      } else {
        std::fprintf(stderr, "Unknown access: %s (use rw or mmap)\n", v);
        return false;
      }
      out->access_overridden = true;
      continue;
    }
    if (arg == "--start") {
      const char* v = need_value(arg.c_str());
      if (!v) return false;
      if (std::strcmp(v, "explicit") == 0) {
        out->explicit_start = true;
      } else if (std::strcmp(v, "auto") == 0) {
        out->explicit_start = false;
      } else {
        std::fprintf(stderr, "Unknown start: %s (use auto or explicit)\n", v);
        return false;
      }
      continue;
    }
    if (arg == "--stdin-raw") {
      out->stdin_raw = true;
      continue;
    }
    if (arg == "--hat-ui") {
      out->hat_ui = true;
      continue;
    }
    if (arg == "--buffer-ms") {
      const char* v = need_value(arg.c_str());
      if (!v) return false;
      out->buffer_ms = std::atoi(v);
      continue;
    }
    if (arg == "--period-ms") {
      const char* v = need_value(arg.c_str());
      if (!v) return false;
      out->period_ms = std::atoi(v);
      continue;
    }
    if (arg == "--ring-ms") {
      const char* v = need_value(arg.c_str());
      if (!v) return false;
      out->ring_ms = std::atoi(v);
      continue;
    }
    if (arg == "--status-ms") {
      const char* v = need_value(arg.c_str());
      if (!v) return false;
      out->status_ms = std::atoi(v);
      continue;
    }

    std::fprintf(stderr, "Unknown arg: %s\n", arg.c_str());
    PrintUsage(argv[0]);
    return false;
  }

  if (out->mic == MicKind::kSpcmic) {
    if (!out->device_overridden) {
      out->device = "hw:CARD=s02E5D5,DEV=0";
    }
    if (!out->channels_overridden) {
      out->channels = 84;
    }
    if (!out->access_overridden) {
      out->access = SND_PCM_ACCESS_RW_INTERLEAVED;
    }
  } else if (out->mic == MicKind::kZylia) {
    if (!out->device_overridden) {
      out->device = "hw:CARD=ZM13E,DEV=0";
    }
    if (!out->channels_overridden) {
      out->channels = 19;
    }
    if (!out->access_overridden) {
      out->access = SND_PCM_ACCESS_MMAP_INTERLEAVED;
    }
  }

  if (out->rate != 48000 && out->rate != 96000) {
    std::fprintf(stderr, "Rate must be 48000 or 96000\n");
    return false;
  }
  if (out->channels <= 0) {
    std::fprintf(stderr, "Channels must be > 0\n");
    return false;
  }
  if (out->buffer_ms <= 0 || out->period_ms <= 0) {
    std::fprintf(stderr, "Buffer/period ms must be > 0\n");
    return false;
  }
  if (out->period_ms >= out->buffer_ms) {
    std::fprintf(stderr, "period-ms should be smaller than buffer-ms\n");
    return false;
  }
  if (out->ring_ms <= 0) {
    std::fprintf(stderr, "ring-ms must be > 0\n");
    return false;
  }
  if (out->status_ms < 0) {
    std::fprintf(stderr, "status-ms must be >= 0\n");
    return false;
  }
  return true;
}

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

#ifdef __linux__

struct UiSnapshot {
  bool recording = false;
  std::string mic;
  unsigned int rate = 0;
  int channels = 0;
  uint64_t xruns = 0;
  uint64_t dropped_bytes = 0;
  int ring_fill_pct = 0;
  uint64_t elapsed_sec = 0;
};

class WaveshareHatUi {
 public:
  const std::string& LastError() const { return last_error_; }

  bool Init() {
    last_error_.clear();

    chip_ = gpiod_chip_open("/dev/gpiochip0");
    if (!chip_) {
      return FailErrno("gpiod_chip_open(/dev/gpiochip0)");
    }

    if (!RequestLine(&dc_, kPinDc, true, 1, "DC")) {
      return false;
    }
    if (!RequestLine(&rst_, kPinRst, true, 1, "RST")) {
      return false;
    }
    if (!RequestLine(&bl_, kPinBl, true, 1, "BL")) {
      return false;
    }
    if (!RequestLine(&btn_[0], kPinKey1, false, 0, "KEY1")) {
      return false;
    }
    if (!RequestLine(&btn_[1], kPinKey2, false, 0, "KEY2")) {
      return false;
    }
    if (!RequestLine(&btn_[2], kPinKey3, false, 0, "KEY3")) {
      return false;
    }
    if (!RequestLine(&btn_[3], kPinUp, false, 0, "UP")) {
      return false;
    }
    if (!RequestLine(&btn_[4], kPinDown, false, 0, "DOWN")) {
      return false;
    }
    if (!RequestLine(&btn_[5], kPinLeft, false, 0, "LEFT")) {
      return false;
    }
    if (!RequestLine(&btn_[6], kPinRight, false, 0, "RIGHT")) {
      return false;
    }
    if (!RequestLine(&btn_[7], kPinPress, false, 0, "PRESS")) {
      return false;
    }

    for (int i = 0; i < 8; ++i) {
      last_btn_[i] = ReadGpio(btn_[i]);
      if (last_btn_[i] < 0) {
        last_btn_[i] = 1;
      }
      idle_btn_[i] = last_btn_[i];
    }

    spi_fd_ = open("/dev/spidev0.0", O_RDWR);
    if (spi_fd_ < 0) {
      return FailErrno("open(/dev/spidev0.0)");
    }
    uint8_t mode = 0;
    uint8_t bits = 8;
    uint32_t speed = 32000000;
    if (ioctl(spi_fd_, SPI_IOC_WR_MODE, &mode) < 0) {
      return FailErrno("ioctl(SPI_IOC_WR_MODE)");
    }
    if (ioctl(spi_fd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) {
      return FailErrno("ioctl(SPI_IOC_WR_BITS_PER_WORD)");
    }
    if (ioctl(spi_fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
      return FailErrno("ioctl(SPI_IOC_WR_MAX_SPEED_HZ)");
    }

    if (!WriteGpio(bl_, 1)) return FailErrno("WriteGpio(BL=1)");
    if (!WriteGpio(rst_, 1)) return FailErrno("WriteGpio(RST=1)");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!WriteGpio(rst_, 0)) return FailErrno("WriteGpio(RST=0)");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!WriteGpio(rst_, 1)) return FailErrno("WriteGpio(RST=1)");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    if (!WriteCmd(0x01, nullptr, 0)) return Fail("WriteCmd(SWRESET)");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    if (!WriteCmd(0x11, nullptr, 0)) return Fail("WriteCmd(SLPOUT)");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const uint8_t madctl[] = {0x00};
    const uint8_t colmod[] = {0x55};  // RGB565
    if (!WriteCmd(0x36, madctl, sizeof(madctl))) return Fail("WriteCmd(MADCTL)");
    if (!WriteCmd(0x3A, colmod, sizeof(colmod))) return Fail("WriteCmd(COLMOD)");
    if (!WriteCmd(0x21, nullptr, 0)) return Fail("WriteCmd(INVON)");
    if (!WriteCmd(0x29, nullptr, 0)) return Fail("WriteCmd(DISPON)");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    Clear(kBlack);
    if (!Flush()) return Fail("Flush(initial)");
    return true;
  }

  void Shutdown() {
    if (spi_fd_ >= 0) close(spi_fd_);
    spi_fd_ = -1;
    ReleaseLine(dc_);
    ReleaseLine(rst_);
    ReleaseLine(bl_);
    for (int i = 0; i < 8; ++i) {
      ReleaseLine(btn_[i]);
    }
    if (chip_) {
      gpiod_chip_close(chip_);
      chip_ = nullptr;
    }
  }

  void PollButtons(bool* start, bool* stop) {
    if (start) *start = false;
    if (stop) *stop = false;
    const int key1 = ReadGpio(btn_[0]);
    const int key2 = ReadGpio(btn_[1]);
    const int key3 = ReadGpio(btn_[2]);
    auto edge = [&](int idx, int value) -> bool {
      return value >= 0 && last_btn_[idx] == idle_btn_[idx] &&
             value != idle_btn_[idx];
    };

    if (stop && edge(0, key1)) {
      *stop = true;
    }
    if (start && edge(1, key2)) {
      *start = true;
    }
    if (edge(2, key3)) {
      backlight_on_ = !backlight_on_;
      WriteGpio(bl_, backlight_on_ ? 1 : 0);
    }
    if (key1 >= 0) last_btn_[0] = key1;
    if (key2 >= 0) last_btn_[1] = key2;
    if (key3 >= 0) last_btn_[2] = key3;
  }

  bool Render(const UiSnapshot& snap) {
    Clear(kBlack);

    FillRect(6, 6, 14, 14, snap.recording ? kRed : kDarkGray);
    DrawText(26, 6, snap.recording ? "REC" : "IDLE", kWhite, 2);

    std::time_t now = std::time(nullptr);
    std::tm tm_now{};
    localtime_r(&now, &tm_now);
    char wall[16];
    std::snprintf(wall, sizeof(wall), "%02d:%02d:%02d", tm_now.tm_hour,
                  tm_now.tm_min, tm_now.tm_sec);
    DrawText(6, 34, "TIME", kCyan, 1);
    DrawText(66, 30, wall, kWhite, 2);

    DrawText(6, 62, "ELAP", kCyan, 1);
    DrawText(66, 58, FormatHms(snap.elapsed_sec), kYellow, 2);

    DrawText(6, 90, "MIC", kCyan, 1);
    DrawText(66, 90, snap.mic, kWhite, 1);

    char rate_ch[32];
    std::snprintf(rate_ch, sizeof(rate_ch), "%uHz  CH:%d", snap.rate,
                  snap.channels);
    DrawText(6, 110, rate_ch, kWhite, 1);

    char xr[32];
    std::snprintf(xr, sizeof(xr), "XRUN:%llu",
                  static_cast<unsigned long long>(snap.xruns));
    DrawText(6, 130, xr, kWhite, 1);

    char dr[32];
    std::snprintf(dr, sizeof(dr), "DROP:%lluMB",
                  static_cast<unsigned long long>(snap.dropped_bytes /
                                                  (1024 * 1024)));
    DrawText(6, 148, dr, kWhite, 1);

    char rg[32];
    std::snprintf(rg, sizeof(rg), "RING:%d%%", snap.ring_fill_pct);
    DrawText(6, 166, rg, kWhite, 1);

    const int bar_w = 220;
    const int bar_fill = (bar_w * snap.ring_fill_pct) / 100;
    FillRect(10, 190, bar_w, 14, kDarkGray);
    if (bar_fill > 0) {
      const uint16_t c =
          (snap.ring_fill_pct < 70) ? kGreen : ((snap.ring_fill_pct < 90) ? kYellow : kRed);
      FillRect(10, 190, bar_fill, 14, c);
    }

    DrawText(6, 212, "KEY1:STOP KEY2:REC KEY3:BL", kDarkGray, 1);
    return Flush();
  }

 private:
  bool Fail(const char* step) {
    last_error_ = step;
    return false;
  }

  bool FailErrno(const std::string& step) {
    const int e = errno;
    std::ostringstream oss;
    oss << step << " failed: errno=" << e << " (" << std::strerror(e) << ")";
    last_error_ = oss.str();
    return false;
  }

  static constexpr int kWidth = 240;
  static constexpr int kHeight = 240;
  static constexpr int kPinRst = 27;
  static constexpr int kPinDc = 25;
  static constexpr int kPinBl = 24;
  static constexpr int kPinKey1 = 21;
  static constexpr int kPinKey2 = 20;
  static constexpr int kPinKey3 = 16;
  static constexpr int kPinUp = 6;
  static constexpr int kPinDown = 19;
  static constexpr int kPinLeft = 5;
  static constexpr int kPinRight = 26;
  static constexpr int kPinPress = 13;

  static constexpr uint16_t kBlack = 0x0000;
  static constexpr uint16_t kWhite = 0xFFFF;
  static constexpr uint16_t kRed = 0xF800;
  static constexpr uint16_t kGreen = 0x07E0;
  static constexpr uint16_t kYellow = 0xFFE0;
  static constexpr uint16_t kCyan = 0x07FF;
  static constexpr uint16_t kDarkGray = 0x39E7;

  bool WriteCmd(uint8_t cmd, const uint8_t* data, size_t data_len) {
    if (!WriteGpio(dc_, 0) || !SpiWrite(&cmd, 1)) {
      return false;
    }
    if (data_len == 0) {
      return true;
    }
    return WriteGpio(dc_, 1) && SpiWrite(data, data_len);
  }

  bool SetWindow() {
    const uint8_t x[] = {0x00, 0x00, 0x00, 0xEF};
    const uint8_t y[] = {0x00, 0x00, 0x00, 0xEF};
    return WriteCmd(0x2A, x, sizeof(x)) && WriteCmd(0x2B, y, sizeof(y)) &&
           WriteCmd(0x2C, nullptr, 0);
  }

  bool Flush() {
    if (!SetWindow()) {
      return false;
    }
    if (!WriteGpio(dc_, 1)) {
      return false;
    }
    return SpiWrite(frame_.data(), frame_.size());
  }

  void Clear(uint16_t color) {
    const uint8_t hi = static_cast<uint8_t>(color >> 8);
    const uint8_t lo = static_cast<uint8_t>(color & 0xFF);
    for (size_t i = 0; i < frame_.size(); i += 2) {
      frame_[i] = hi;
      frame_[i + 1] = lo;
    }
  }

  void SetPixel(int x, int y, uint16_t color) {
    if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return;
    // Rotate full UI 90 degrees clockwise in software.
    const int rx = (kWidth - 1) - y;
    const int ry = x;
    const size_t idx = static_cast<size_t>(ry * kWidth + rx) * 2;
    frame_[idx] = static_cast<uint8_t>(color >> 8);
    frame_[idx + 1] = static_cast<uint8_t>(color & 0xFF);
  }

  void FillRect(int x, int y, int w, int h, uint16_t color) {
    for (int yy = y; yy < y + h; ++yy) {
      for (int xx = x; xx < x + w; ++xx) {
        SetPixel(xx, yy, color);
      }
    }
  }

  static const uint8_t* Glyph(char c) {
    static const uint8_t sp[] = {0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t dash[] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t colon[] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t p0[] = {0x3E, 0x51, 0x49, 0x45, 0x3E};
    static const uint8_t p1[] = {0x00, 0x42, 0x7F, 0x40, 0x00};
    static const uint8_t p2[] = {0x42, 0x61, 0x51, 0x49, 0x46};
    static const uint8_t p3[] = {0x21, 0x41, 0x45, 0x4B, 0x31};
    static const uint8_t p4[] = {0x18, 0x14, 0x12, 0x7F, 0x10};
    static const uint8_t p5[] = {0x27, 0x45, 0x45, 0x45, 0x39};
    static const uint8_t p6[] = {0x3C, 0x4A, 0x49, 0x49, 0x30};
    static const uint8_t p7[] = {0x01, 0x71, 0x09, 0x05, 0x03};
    static const uint8_t p8[] = {0x36, 0x49, 0x49, 0x49, 0x36};
    static const uint8_t p9[] = {0x06, 0x49, 0x49, 0x29, 0x1E};
    static const uint8_t a[] = {0x7E, 0x11, 0x11, 0x11, 0x7E};
    static const uint8_t b[] = {0x7F, 0x49, 0x49, 0x49, 0x36};
    static const uint8_t c2[] = {0x3E, 0x41, 0x41, 0x41, 0x22};
    static const uint8_t d[] = {0x7F, 0x41, 0x41, 0x22, 0x1C};
    static const uint8_t e[] = {0x7F, 0x49, 0x49, 0x49, 0x41};
    static const uint8_t g[] = {0x3E, 0x41, 0x49, 0x49, 0x7A};
    static const uint8_t h[] = {0x7F, 0x08, 0x08, 0x08, 0x7F};
    static const uint8_t i[] = {0x00, 0x41, 0x7F, 0x41, 0x00};
    static const uint8_t k[] = {0x7F, 0x08, 0x14, 0x22, 0x41};
    static const uint8_t l[] = {0x7F, 0x40, 0x40, 0x40, 0x40};
    static const uint8_t m[] = {0x7F, 0x02, 0x0C, 0x02, 0x7F};
    static const uint8_t n[] = {0x7F, 0x04, 0x08, 0x10, 0x7F};
    static const uint8_t o[] = {0x3E, 0x41, 0x41, 0x41, 0x3E};
    static const uint8_t p[] = {0x7F, 0x09, 0x09, 0x09, 0x06};
    static const uint8_t r[] = {0x7F, 0x09, 0x19, 0x29, 0x46};
    static const uint8_t s[] = {0x46, 0x49, 0x49, 0x49, 0x31};
    static const uint8_t t[] = {0x01, 0x01, 0x7F, 0x01, 0x01};
    static const uint8_t u[] = {0x3F, 0x40, 0x40, 0x40, 0x3F};
    static const uint8_t x[] = {0x63, 0x14, 0x08, 0x14, 0x63};
    static const uint8_t y[] = {0x03, 0x04, 0x78, 0x04, 0x03};
    static const uint8_t z[] = {0x61, 0x51, 0x49, 0x45, 0x43};

    switch (c) {
      case '0': return p0;
      case '1': return p1;
      case '2': return p2;
      case '3': return p3;
      case '4': return p4;
      case '5': return p5;
      case '6': return p6;
      case '7': return p7;
      case '8': return p8;
      case '9': return p9;
      case 'A': return a;
      case 'B': return b;
      case 'C': return c2;
      case 'D': return d;
      case 'E': return e;
      case 'G': return g;
      case 'H': return h;
      case 'I': return i;
      case 'K': return k;
      case 'L': return l;
      case 'M': return m;
      case 'N': return n;
      case 'O': return o;
      case 'P': return p;
      case 'R': return r;
      case 'S': return s;
      case 'T': return t;
      case 'U': return u;
      case 'X': return x;
      case 'Y': return y;
      case 'Z': return z;
      case ':': return colon;
      case '-': return dash;
      case ' ': return sp;
      default: return sp;
    }
  }

  void DrawText(int x, int y, const std::string& text, uint16_t color,
                int scale) {
    int cx = x;
    for (char raw : text) {
      char c = raw;
      if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
      }
      const uint8_t* g = Glyph(c);
      for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
          if ((g[col] >> row) & 0x01) {
            FillRect(cx + col * scale, y + row * scale, scale, scale, color);
          }
        }
      }
      cx += 6 * scale;
    }
  }

  struct GpioLine {
    unsigned int offset = 0;
    gpiod_line_request* request = nullptr;
  };

  bool RequestLine(GpioLine* line, unsigned int offset, bool output,
                   int initial_value, const char* label) {
    gpiod_line_settings* settings = gpiod_line_settings_new();
    if (!settings) {
      return FailErrno((std::string("gpiod_line_settings_new(") + label + ")")
                           .c_str());
    }
    gpiod_line_settings_set_direction(
        settings, output ? GPIOD_LINE_DIRECTION_OUTPUT
                         : GPIOD_LINE_DIRECTION_INPUT);
    if (output) {
      gpiod_line_settings_set_output_value(
          settings, initial_value ? GPIOD_LINE_VALUE_ACTIVE
                                  : GPIOD_LINE_VALUE_INACTIVE);
    } else {
      // Waveshare key/joystick lines are switch-to-GND inputs; request pull-up.
      (void)gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);
    }

    gpiod_line_config* line_cfg = gpiod_line_config_new();
    if (!line_cfg) {
      gpiod_line_settings_free(settings);
      return FailErrno((std::string("gpiod_line_config_new(") + label + ")")
                           .c_str());
    }
    const unsigned int offsets[1] = {offset};
    if (gpiod_line_config_add_line_settings(line_cfg, offsets, 1, settings) <
        0) {
      gpiod_line_config_free(line_cfg);
      gpiod_line_settings_free(settings);
      return FailErrno((std::string("gpiod_line_config_add_line_settings(") +
                        label + ")")
                           .c_str());
    }

    gpiod_request_config* req_cfg = gpiod_request_config_new();
    if (!req_cfg) {
      gpiod_line_config_free(line_cfg);
      gpiod_line_settings_free(settings);
      return FailErrno((std::string("gpiod_request_config_new(") + label + ")")
                           .c_str());
    }
    gpiod_request_config_set_consumer(req_cfg, "rpi_multirec");

    gpiod_line_request* req = gpiod_chip_request_lines(chip_, req_cfg, line_cfg);
    gpiod_request_config_free(req_cfg);
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);
    if (!req) {
      return FailErrno((std::string("gpiod_chip_request_lines(") + label + ")")
                           .c_str());
    }

    line->offset = offset;
    line->request = req;
    return true;
  }

  static void ReleaseLine(GpioLine& line) {
    if (line.request) {
      gpiod_line_request_release(line.request);
      line.request = nullptr;
    }
  }

  static bool WriteGpio(const GpioLine& line, int value) {
    if (!line.request) return false;
    const enum gpiod_line_value v =
        value ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
    return gpiod_line_request_set_value(
               line.request, line.offset,
               static_cast<enum gpiod_line_value>(v)) == 0;
  }

  static int ReadGpio(const GpioLine& line) {
    if (!line.request) return -1;
    const int v = gpiod_line_request_get_value(line.request, line.offset);
    if (v < 0) {
      return -1;
    }
    return (v == GPIOD_LINE_VALUE_ACTIVE) ? 1 : 0;
  }

  bool SpiWrite(const uint8_t* data, size_t len) {
    if (spi_fd_ < 0) return false;
    // spidev has a per-transfer size limit (often 4096 bytes), so send in chunks.
    constexpr size_t kSpiChunkBytes = 4096;
    size_t offset = 0;
    while (offset < len) {
      const size_t chunk = std::min(kSpiChunkBytes, len - offset);
      spi_ioc_transfer tr{};
      tr.tx_buf = reinterpret_cast<unsigned long>(data + offset);
      tr.len = static_cast<uint32_t>(chunk);
      tr.speed_hz = 32000000;
      tr.bits_per_word = 8;
      if (ioctl(spi_fd_, SPI_IOC_MESSAGE(1), &tr) < 0) {
        return false;
      }
      offset += chunk;
    }
    return true;
  }

  int spi_fd_ = -1;
  gpiod_chip* chip_ = nullptr;
  GpioLine dc_;
  GpioLine rst_;
  GpioLine bl_;
  GpioLine btn_[8];
  int last_btn_[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  int idle_btn_[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  bool backlight_on_ = true;
  std::string last_error_;
  std::vector<uint8_t> frame_ =
      std::vector<uint8_t>(static_cast<size_t>(kWidth * kHeight * 2), 0);
};

#else

struct UiSnapshot {
  bool recording = false;
  std::string mic;
  unsigned int rate = 0;
  int channels = 0;
  uint64_t xruns = 0;
  uint64_t dropped_bytes = 0;
  int ring_fill_pct = 0;
  uint64_t elapsed_sec = 0;
};

class WaveshareHatUi {
 public:
  bool Init() { return false; }
  const std::string& LastError() const { return last_error_; }
  void Shutdown() {}
  void PollButtons(bool* start, bool* stop) {
    if (start) *start = false;
    if (stop) *stop = false;
  }
  bool Render(const UiSnapshot&) { return false; }
 private:
  std::string last_error_ = "Not supported on non-Linux build";
};

#endif

void ListAlsaDevices() {
  void** hints = nullptr;
  const int rc = snd_device_name_hint(-1, "pcm", &hints);
  if (rc < 0) {
    std::fprintf(stderr, "snd_device_name_hint failed: %s\n",
                 snd_strerror(rc));
    return;
  }

  std::printf("ALSA PCM devices:\n");
  for (void** it = hints; it && *it; ++it) {
    char* name = snd_device_name_get_hint(*it, "NAME");
    char* desc = snd_device_name_get_hint(*it, "DESC");
    char* ioid = snd_device_name_get_hint(*it, "IOID");

    if (name) {
      const char* io = ioid ? ioid : "Unknown";
      std::printf("- %s [%s]\n", name, io);
      if (desc) {
        std::printf("  %s\n", desc);
      }
    }

    if (name) std::free(name);
    if (desc) std::free(desc);
    if (ioid) std::free(ioid);
  }

  snd_device_name_free_hint(hints);
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!ParseArgs(argc, argv, &opt)) {
    return 1;
  }

  if (opt.show_help) {
    PrintUsage(argv[0]);
    return 0;
  }

  if (opt.list_devices) {
    ListAlsaDevices();
    return 0;
  }

  if (opt.hat_ui && opt.stdin_raw) {
    std::fprintf(stderr, "--hat-ui does not support --stdin-raw\n");
    return 1;
  }

  if (!opt.out_overridden) {
    if (opt.mic == MicKind::kUnspecified) {
      std::fprintf(stderr,
                   "Auto filename requires --mic spcmic|zylia "
                   "(or provide --out)\n");
      return 1;
    }
    if (!opt.hat_ui) {
      opt.out_path = BuildAutoOutPath(opt.mic);
    }
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  snd_pcm_t* pcm = nullptr;
  snd_pcm_uframes_t period_size = 0;
  snd_pcm_uframes_t buffer_size = 0;
  unsigned int actual_rate = static_cast<unsigned int>(opt.rate);

  if (!opt.stdin_raw) {
    int err = snd_pcm_open(&pcm, opt.device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
      std::fprintf(stderr, "snd_pcm_open failed: %s\n", snd_strerror(err));
      return 1;
    }

    snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_malloc(&params);
    snd_pcm_hw_params_any(pcm, params);

    auto Check = [](int rc, const char* what) -> bool {
      if (rc < 0) {
        std::fprintf(stderr, "%s failed: %s\n", what, snd_strerror(rc));
        return false;
      }
      return true;
    };

    if (!Check(snd_pcm_hw_params_set_access(pcm, params, opt.access),
               "snd_pcm_hw_params_set_access") ||
        !Check(snd_pcm_hw_params_set_format(pcm, params, opt.format),
               "snd_pcm_hw_params_set_format") ||
        !Check(snd_pcm_hw_params_set_channels(pcm, params, opt.channels),
               "snd_pcm_hw_params_set_channels")) {
      snd_pcm_hw_params_free(params);
      snd_pcm_close(pcm);
      return 1;
    }

    unsigned int rate = static_cast<unsigned int>(opt.rate);
    // Rate resample may not be supported on all devices; ignore if it fails.
    snd_pcm_hw_params_set_rate_resample(pcm, params, 0);
    if (!Check(snd_pcm_hw_params_set_rate_near(pcm, params, &rate, nullptr),
               "snd_pcm_hw_params_set_rate_near")) {
      snd_pcm_hw_params_free(params);
      snd_pcm_close(pcm);
      return 1;
    }

    unsigned int period_time = static_cast<unsigned int>(opt.period_ms) * 1000;
    if (!Check(snd_pcm_hw_params_set_period_time_near(
                   pcm, params, &period_time, nullptr),
               "snd_pcm_hw_params_set_period_time_near")) {
      snd_pcm_hw_params_free(params);
      snd_pcm_close(pcm);
      return 1;
    }

    unsigned int buffer_time = static_cast<unsigned int>(opt.buffer_ms) * 1000;
    if (!Check(snd_pcm_hw_params_set_buffer_time_near(
                   pcm, params, &buffer_time, nullptr),
               "snd_pcm_hw_params_set_buffer_time_near")) {
      snd_pcm_hw_params_free(params);
      snd_pcm_close(pcm);
      return 1;
    }

    err = snd_pcm_hw_params(pcm, params);
    if (err < 0) {
      std::fprintf(stderr, "snd_pcm_hw_params failed: %s\n", snd_strerror(err));
      snd_pcm_hw_params_free(params);
      snd_pcm_close(pcm);
      return 1;
    }

    snd_pcm_hw_params_get_period_size(params, &period_size, nullptr);
    snd_pcm_hw_params_get_buffer_size(params, &buffer_size);
    snd_pcm_hw_params_get_rate(params, &actual_rate, nullptr);
    snd_pcm_hw_params_free(params);

    if (actual_rate != static_cast<unsigned int>(opt.rate)) {
      std::fprintf(stderr, "Warning: requested rate %d, got %u\n", opt.rate,
                   actual_rate);
    }
  } else {
    const uint64_t frames =
        (static_cast<uint64_t>(opt.rate) * opt.period_ms) / 1000;
    period_size = static_cast<snd_pcm_uframes_t>(frames > 0 ? frames : 1);
    buffer_size = period_size * 4;
  }

  const int bytes_per_sample =
      (opt.format == SND_PCM_FORMAT_S16_LE) ? 2 : 3;
  const int bytes_per_frame = opt.channels * bytes_per_sample;

  if (period_size == 0) {
    std::fprintf(stderr, "Invalid period size\n");
    if (pcm) {
      snd_pcm_close(pcm);
    }
    return 1;
  }

  std::vector<uint8_t> buffer;
  buffer.resize(static_cast<size_t>(period_size) * bytes_per_frame);

  SF_INFO info_template{};
  info_template.samplerate = static_cast<int>(actual_rate);
  info_template.channels = opt.channels;
  info_template.format = SF_FORMAT_RF64 |
                         ((opt.format == SND_PCM_FORMAT_S16_LE)
                              ? SF_FORMAT_PCM_16
                              : SF_FORMAT_PCM_24);

  size_t ring_bytes = static_cast<size_t>(actual_rate) * bytes_per_frame *
                      static_cast<size_t>(opt.ring_ms) / 1000;
  const size_t period_bytes =
      static_cast<size_t>(period_size) * bytes_per_frame;
  if (ring_bytes < period_bytes * 2) {
    ring_bytes = period_bytes * 2;
  }
  ring_bytes -= ring_bytes % bytes_per_frame;

  RingBuffer ring(ring_bytes);
  std::mutex ring_mutex;
  std::condition_variable ring_cv;

  SNDFILE* snd = nullptr;
  std::thread writer;
  std::atomic<bool> writer_running{false};
  std::atomic<bool> writer_error{false};

  std::atomic<uint64_t> dropped_bytes{0};
  std::atomic<uint64_t> xrun_count{0};
  std::atomic<bool> recording_active{false};
  std::atomic<bool> start_requested{false};
  std::atomic<bool> stop_requested{false};
  std::atomic<int64_t> record_start_ms{0};

  uint64_t take_count = 0;
  std::string current_out_path;

  const char* fmt_label =
      (opt.format == SND_PCM_FORMAT_S16_LE) ? "S16_LE" : "S24_3LE";
  const char* access_label =
      (opt.access == SND_PCM_ACCESS_MMAP_INTERLEAVED) ? "MMAP" : "RW";
  const char* start_label = opt.explicit_start ? "explicit" : "auto";
  const std::string out_preview =
      opt.out_overridden
          ? opt.out_path
          : (std::string(MicKindToString(opt.mic)) + "_YYYYMMDD_HHMMSS.rf64");

  if (opt.hat_ui) {
    std::fprintf(stdout,
                 "Idle mode enabled on HAT UI.\n"
                 "ALSA device: %s | period: %lu frames | buffer: %lu frames\n"
                 "Format: %d ch @ %u Hz (%s) | access: %s | start: %s\n"
                 "Output file: %s\n"
                 "Ring buffer: %lu ms (~%lu MB)\n"
                 "Press KEY2 to start, KEY1 to stop, Ctrl+C to exit.\n",
                 opt.device.c_str(), static_cast<unsigned long>(period_size),
                 static_cast<unsigned long>(buffer_size), opt.channels,
                 actual_rate, fmt_label, access_label, start_label,
                 out_preview.c_str(), static_cast<unsigned long>(opt.ring_ms),
                 static_cast<unsigned long>(ring_bytes / (1024 * 1024)));
  } else if (opt.stdin_raw) {
    std::fprintf(stdout,
                 "Recording %d ch @ %u Hz (%s) to %s\n"
                 "Input: stdin raw PCM | period: %lu frames\n"
                 "Ring buffer: %lu ms (~%lu MB)\n"
                 "Press Ctrl+C to stop...\n",
                 opt.channels, actual_rate, fmt_label, opt.out_path.c_str(),
                 static_cast<unsigned long>(period_size),
                 static_cast<unsigned long>(opt.ring_ms),
                 static_cast<unsigned long>(ring_bytes / (1024 * 1024)));
  } else {
    std::fprintf(stdout,
                 "Recording %d ch @ %u Hz (%s) to %s\n"
                 "ALSA device: %s | period: %lu frames | buffer: %lu frames\n"
                 "Access: %s | start: %s\n"
                 "Ring buffer: %lu ms (~%lu MB)\n"
                 "Press Ctrl+C to stop...\n",
                 opt.channels, actual_rate, fmt_label, opt.out_path.c_str(),
                 opt.device.c_str(), static_cast<unsigned long>(period_size),
                 static_cast<unsigned long>(buffer_size), access_label,
                 start_label, static_cast<unsigned long>(opt.ring_ms),
                 static_cast<unsigned long>(ring_bytes / (1024 * 1024)));
  }

  if (!opt.stdin_raw) {
    snd_pcm_sw_params_t* swparams = nullptr;
    snd_pcm_sw_params_malloc(&swparams);
    snd_pcm_sw_params_current(pcm, swparams);
    snd_pcm_sw_params_set_start_threshold(pcm, swparams, 1);
    snd_pcm_sw_params_set_avail_min(pcm, swparams, period_size);
    const int err = snd_pcm_sw_params(pcm, swparams);
    snd_pcm_sw_params_free(swparams);
    if (err < 0) {
      std::fprintf(stderr, "snd_pcm_sw_params failed: %s\n",
                   snd_strerror(err));
      snd_pcm_close(pcm);
      return 1;
    }
  }

  auto next_status = std::chrono::steady_clock::now();
  const auto status_interval = std::chrono::milliseconds(opt.status_ms);
  auto now_ms = []() -> int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };

  auto start_writer = [&]() {
    writer_running.store(true);
    writer = std::thread([&] {
      std::vector<uint8_t> out;
      out.resize(period_bytes);
      for (;;) {
        size_t bytes = 0;
        {
          std::unique_lock<std::mutex> lock(ring_mutex);
          ring_cv.wait_for(lock, std::chrono::milliseconds(200), [&] {
            return ring.size() > 0 || !writer_running.load();
          });
          if (!writer_running.load() && ring.size() == 0) {
            break;
          }
          bytes = ring.Read(out.data(), out.size());
        }
        if (bytes == 0) {
          continue;
        }
        const sf_count_t wrote = sf_write_raw(snd, out.data(), bytes);
        if (wrote != static_cast<sf_count_t>(bytes)) {
          std::fprintf(stderr, "sf_write_raw short write: %ld/%ld bytes\n",
                       static_cast<long>(wrote), static_cast<long>(bytes));
          writer_error.store(true);
          writer_running.store(false);
          g_running.store(false);
          ring_cv.notify_all();
          break;
        }
      }
    });
  };

  auto stop_writer = [&]() {
    writer_running.store(false);
    ring_cv.notify_all();
    if (writer.joinable()) {
      writer.join();
    }
  };

  auto start_take = [&]() -> bool {
    if (recording_active.load()) {
      return true;
    }

    const int take_index = static_cast<int>(take_count + 1);
    if (!opt.hat_ui) {
      current_out_path = opt.out_path;
    } else {
      current_out_path =
          opt.out_overridden ? BuildManualTakePath(opt.out_path, take_index)
                             : BuildAutoOutPath(opt.mic);
    }

    {
      std::lock_guard<std::mutex> lock(ring_mutex);
      ring.Clear();
    }
    dropped_bytes.store(0);
    xrun_count.store(0);

    SF_INFO info = info_template;
    snd = sf_open(current_out_path.c_str(), SFM_WRITE, &info);
    if (!snd) {
      std::fprintf(stderr, "sf_open failed for %s: %s\n",
                   current_out_path.c_str(), sf_strerror(nullptr));
      return false;
    }
    sf_command(snd, SFC_RF64_AUTO_DOWNGRADE, nullptr, SF_TRUE);

    if (!opt.stdin_raw) {
      int err = snd_pcm_prepare(pcm);
      if (err < 0) {
        std::fprintf(stderr, "snd_pcm_prepare failed: %s\n", snd_strerror(err));
        sf_close(snd);
        snd = nullptr;
        return false;
      }
      if (opt.explicit_start) {
        err = snd_pcm_start(pcm);
        if (err < 0) {
          std::fprintf(stderr, "snd_pcm_start failed: %s\n", snd_strerror(err));
          sf_close(snd);
          snd = nullptr;
          return false;
        }
      }
    }

    start_writer();
    recording_active.store(true);
    record_start_ms.store(now_ms());
    next_status = std::chrono::steady_clock::now();
    ++take_count;
    std::fprintf(stdout, "Recording started: %s\n", current_out_path.c_str());
    return true;
  };

  auto stop_take = [&]() {
    if (!recording_active.load()) {
      return;
    }

    if (!opt.stdin_raw && pcm) {
      snd_pcm_drop(pcm);
    }

    recording_active.store(false);
    record_start_ms.store(0);
    stop_writer();

    if (snd) {
      sf_write_sync(snd);
      sf_close(snd);
      snd = nullptr;
    }
    {
      std::lock_guard<std::mutex> lock(ring_mutex);
      ring.Clear();
    }
    std::fprintf(stdout, "Take stopped: %s | XRUNs: %llu | Dropped: %llu bytes\n",
                 current_out_path.c_str(),
                 static_cast<unsigned long long>(xrun_count.load()),
                 static_cast<unsigned long long>(dropped_bytes.load()));
  };

  const std::string ui_mic_name =
      (opt.mic == MicKind::kUnspecified) ? "custom" : MicKindToString(opt.mic);
  auto make_ui_snapshot = [&]() -> UiSnapshot {
    UiSnapshot snap;
    snap.recording = recording_active.load();
    snap.mic = ui_mic_name;
    snap.rate = actual_rate;
    snap.channels = opt.channels;
    snap.xruns = xrun_count.load();
    snap.dropped_bytes = dropped_bytes.load();
    {
      std::lock_guard<std::mutex> lock(ring_mutex);
      if (ring.capacity() > 0) {
        const size_t pct = (ring.size() * 100) / ring.capacity();
        snap.ring_fill_pct = static_cast<int>(std::min<size_t>(pct, 100));
      }
    }
    if (snap.recording) {
      const int64_t start_ms = record_start_ms.load();
      if (start_ms > 0) {
        const int64_t delta_ms = std::max<int64_t>(0, now_ms() - start_ms);
        snap.elapsed_sec = static_cast<uint64_t>(delta_ms / 1000);
      }
    }
    return snap;
  };

  std::unique_ptr<WaveshareHatUi> hat_ui;
  std::thread ui_thread;
  if (opt.hat_ui) {
#ifdef __linux__
    hat_ui = std::make_unique<WaveshareHatUi>();
    if (!hat_ui->Init()) {
      std::fprintf(
          stderr,
          "Failed to initialize Waveshare HAT UI: %s\n"
          "Check SPI (/dev/spidev0.0), GPIO permissions, and wiring.\n"
          "Try running with sudo.\n",
          hat_ui->LastError().c_str());
      if (pcm) {
        snd_pcm_close(pcm);
      }
      return 1;
    }
    ui_thread = std::thread([&] {
      while (g_running.load()) {
        bool start_evt = false;
        bool stop_evt = false;
        hat_ui->PollButtons(&start_evt, &stop_evt);
        if (start_evt) {
          start_requested.store(true);
        }
        if (stop_evt) {
          stop_requested.store(true);
        }
        hat_ui->Render(make_ui_snapshot());
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
      }
      UiSnapshot snap = make_ui_snapshot();
      snap.recording = false;
      hat_ui->Render(snap);
    });
#else
    std::fprintf(stderr, "--hat-ui is only supported on Linux.\n");
    if (pcm) {
      snd_pcm_close(pcm);
    }
    return 1;
#endif
  }

  if (!opt.hat_ui) {
    if (!start_take()) {
      g_running.store(false);
    }
  }

  while (g_running.load()) {
    if (opt.hat_ui && !recording_active.load()) {
      if (start_requested.exchange(false)) {
        if (!start_take()) {
          std::fprintf(stderr, "Failed to start take\n");
        }
      }
      stop_requested.store(false);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    if (opt.hat_ui && recording_active.load() &&
        stop_requested.exchange(false)) {
      stop_take();
      continue;
    }

    if (!recording_active.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    if (opt.stdin_raw) {
      const size_t want = period_bytes;
      const size_t got = std::fread(buffer.data(), 1, want, stdin);
      if (got == 0) {
        if (std::ferror(stdin)) {
          std::perror("stdin read");
        }
        g_running.store(false);
        break;
      }
      const size_t frames = got / bytes_per_frame;
      const size_t bytes = frames * bytes_per_frame;
      if (bytes == 0) {
        continue;
      }

      bool pushed = false;
      {
        std::lock_guard<std::mutex> lock(ring_mutex);
        if (ring.free() >= bytes) {
          ring.Write(buffer.data(), bytes);
          pushed = true;
        }
      }
      if (!pushed) {
        dropped_bytes.fetch_add(bytes);
      } else {
        ring_cv.notify_one();
      }
    } else {
      snd_pcm_sframes_t frames =
          (opt.access == SND_PCM_ACCESS_MMAP_INTERLEAVED)
              ? snd_pcm_mmap_readi(pcm, buffer.data(), period_size)
              : snd_pcm_readi(pcm, buffer.data(), period_size);
      if (frames == -EPIPE) {
        xrun_count.fetch_add(1);
        std::fprintf(stderr, "XRUN (overrun) detected\n");
        snd_pcm_prepare(pcm);
        continue;
      }
      if (frames < 0) {
        const int state = snd_pcm_state(pcm);
        std::fprintf(stderr, "snd_pcm_readi error: %s (state=%s)\n",
                     snd_strerror(static_cast<int>(frames)),
                     snd_pcm_state_name(static_cast<snd_pcm_state_t>(state)));
        snd_pcm_status_t* status = nullptr;
        snd_pcm_status_malloc(&status);
        if (status && snd_pcm_status(pcm, status) == 0) {
          const snd_pcm_sframes_t avail = snd_pcm_status_get_avail(status);
          const snd_pcm_sframes_t delay = snd_pcm_status_get_delay(status);
          std::fprintf(stderr, "  status: avail=%ld delay=%ld\n",
                       static_cast<long>(avail), static_cast<long>(delay));
        }
        if (status) {
          snd_pcm_status_free(status);
        }
        frames = snd_pcm_recover(pcm, static_cast<int>(frames), 1);
        if (frames < 0) {
          std::fprintf(stderr, "snd_pcm_readi failed: %s\n",
                       snd_strerror(static_cast<int>(frames)));
          g_running.store(false);
          break;
        }
        continue;
      }
      if (frames == 0) {
        continue;
      }

      const size_t bytes = static_cast<size_t>(frames) * bytes_per_frame;
      bool pushed = false;
      {
        std::lock_guard<std::mutex> lock(ring_mutex);
        if (ring.free() >= bytes) {
          ring.Write(buffer.data(), bytes);
          pushed = true;
        }
      }
      if (!pushed) {
        dropped_bytes.fetch_add(bytes);
      } else {
        ring_cv.notify_one();
      }
    }

    if (opt.status_ms > 0) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_status) {
        const uint64_t xruns = xrun_count.load();
        const uint64_t dropped = dropped_bytes.load();
        size_t ring_used = 0;
        {
          std::lock_guard<std::mutex> lock(ring_mutex);
          ring_used = ring.size();
        }
        const double ring_fill =
            ring.capacity() == 0
                ? 0.0
                : (100.0 * static_cast<double>(ring_used) /
                   static_cast<double>(ring.capacity()));
        std::fprintf(stderr,
                     "[status] xruns=%llu dropped=%lluB ring=%.1f%%\n",
                     static_cast<unsigned long long>(xruns),
                     static_cast<unsigned long long>(dropped), ring_fill);
        next_status = now + status_interval;
      }
    }
  }

  if (recording_active.load()) {
    stop_take();
  }
  stop_writer();

  g_running.store(false);
  ring_cv.notify_all();
  if (ui_thread.joinable()) {
    ui_thread.join();
  }
  if (hat_ui) {
    hat_ui->Shutdown();
  }
  if (snd) {
    sf_close(snd);
  }
  if (pcm) {
    snd_pcm_close(pcm);
  }
  std::fprintf(stdout,
               "Stopped. Takes: %llu | Last XRUNs: %llu | Last Dropped: %llu "
               "bytes\n",
               static_cast<unsigned long long>(take_count),
               static_cast<unsigned long long>(xrun_count.load()),
               static_cast<unsigned long long>(dropped_bytes.load()));
  if (writer_error.load()) {
    return 1;
  }
  return 0;
}

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
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <gpiod.h>
#include <linux/i2c-dev.h>
#include <linux/spi/spidev.h>
#include <sys/statvfs.h>
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
constexpr const char* kDefaultRecordingsDir = "/srv/rpi_multirec/recordings";

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
  oss << kDefaultRecordingsDir << "/" << mic_name << "_"
      << std::put_time(&tm_now, "%Y%m%d_%H%M%S") << ".rf64";
  return oss.str();
}

std::string EnsureRecordingsPath(const std::string& path) {
  if (path.empty()) {
    return path;
  }
  const std::filesystem::path p(path);
  if (p.is_absolute()) {
    return path;
  }
  return (std::filesystem::path(kDefaultRecordingsDir) / p).string();
}

bool EnsureParentDirectoryExists(const std::string& file_path,
                                 std::string* error) {
  try {
    const std::filesystem::path p(file_path);
    const std::filesystem::path parent = p.parent_path();
    if (parent.empty()) {
      return true;
    }
    std::filesystem::create_directories(parent);
    return true;
  } catch (const std::exception& e) {
    if (error) {
      *error = e.what();
    }
    return false;
  }
}

bool GetFreeBytesForPath(const std::string& file_path, uint64_t* free_bytes) {
  if (!free_bytes) {
    return false;
  }
#ifdef __linux__
  try {
    std::filesystem::path p(file_path);
    std::filesystem::path dir = p;
    if (!std::filesystem::is_directory(dir)) {
      dir = p.parent_path();
    }
    if (dir.empty()) {
      dir = std::filesystem::path(kDefaultRecordingsDir);
    }
    struct statvfs s {};
    if (statvfs(dir.c_str(), &s) != 0) {
      return false;
    }
    *free_bytes =
        static_cast<uint64_t>(s.f_bavail) * static_cast<uint64_t>(s.f_frsize);
    return true;
  } catch (...) {
    return false;
  }
#else
  (void)file_path;
  return false;
#endif
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
      "  Default output directory: /srv/rpi_multirec/recordings\n"
      "  Auto naming requires --mic spcmic|zylia.\n"
      "\n"
      "Waveshare HAT controls:\n"
      "  With --hat-ui the app starts in IDLE.\n"
      "  KEY2 = MON from IDLE, then KEY2 again = REC\n"
      "  KEY1 = stop (MON->IDLE or REC->stop/finalize)\n"
      "  KEY3 = backlight toggle\n"
      "  Joystick LEFT/RIGHT (IDLE only) = select spcmic/zylia preset\n"
      "  SPCMIC: Joystick UP/DOWN (IDLE only) = select 96kHz/48kHz\n"
      "  ZYLIA: Joystick UP/DOWN (IDLE/MON/REC) = Master Gain +/-1 dB (hold to repeat)\n"
      "  Zylia sample rate is fixed to 48kHz.\n",
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
    if (out->rate != 48000) {
      std::fprintf(stderr,
                   "Info: forcing rate to 48000 for zylia preset (requested %d)\n",
                   out->rate);
      out->rate = 48000;
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

class ZyliaGainControl {
 public:
  ~ZyliaGainControl() { Close(); }

  bool Open(const std::string& card = "hw:CARD=ZM13E") {
    Close();

    if (snd_mixer_open(&mixer_, 0) < 0) {
      Close();
      return false;
    }
    if (snd_mixer_attach(mixer_, card.c_str()) < 0) {
      Close();
      return false;
    }
    if (snd_mixer_selem_register(mixer_, nullptr, nullptr) < 0) {
      Close();
      return false;
    }
    if (snd_mixer_load(mixer_) < 0) {
      Close();
      return false;
    }

    snd_mixer_selem_id_t* sid = nullptr;
    snd_mixer_selem_id_malloc(&sid);
    if (!sid) {
      Close();
      return false;
    }
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "Master Gain");
    elem_ = snd_mixer_find_selem(mixer_, sid);
    snd_mixer_selem_id_free(sid);
    if (!elem_) {
      Close();
      return false;
    }

    if (snd_mixer_selem_has_capture_volume(elem_)) {
      use_capture_ = true;
      if (snd_mixer_selem_get_capture_dB_range(elem_, &min_db_, &max_db_) < 0) {
        Close();
        return false;
      }
    } else if (snd_mixer_selem_has_playback_volume(elem_)) {
      use_capture_ = false;
      if (snd_mixer_selem_get_playback_dB_range(elem_, &min_db_, &max_db_) < 0) {
        Close();
        return false;
      }
    } else {
      Close();
      return false;
    }
    if (min_db_ > max_db_) {
      std::swap(min_db_, max_db_);
    }
    return true;
  }

  void Close() {
    elem_ = nullptr;
    if (mixer_) {
      snd_mixer_close(mixer_);
      mixer_ = nullptr;
    }
    use_capture_ = false;
    min_db_ = 0;
    max_db_ = 0;
  }

  bool IsOpen() const { return elem_ != nullptr; }

  bool ReadDb(int* db) const {
    if (!db) {
      return false;
    }
    long centi_db = 0;
    if (!GetDbCenti(&centi_db)) {
      return false;
    }
    *db = static_cast<int>((centi_db >= 0) ? ((centi_db + 50) / 100)
                                           : ((centi_db - 50) / 100));
    return true;
  }

  bool StepDb(int delta_db) {
    if (delta_db == 0 || !IsOpen()) {
      return delta_db == 0;
    }
    long centi_db = 0;
    if (!GetDbCenti(&centi_db)) {
      return false;
    }
    long target = centi_db + static_cast<long>(delta_db) * 100;
    target = std::max(min_db_, std::min(max_db_, target));
    if (target == centi_db) {
      return true;
    }
    return SetDbCenti(target);
  }

 private:
  bool GetDbCenti(long* centi_db) const {
    if (!centi_db || !elem_) {
      return false;
    }
    if (use_capture_) {
      const snd_mixer_selem_channel_id_t ch =
          snd_mixer_selem_is_capture_mono(elem_) ? SND_MIXER_SCHN_MONO
                                                 : SND_MIXER_SCHN_FRONT_LEFT;
      return snd_mixer_selem_get_capture_dB(elem_, ch, centi_db) == 0;
    }
    const snd_mixer_selem_channel_id_t ch =
        snd_mixer_selem_is_playback_mono(elem_) ? SND_MIXER_SCHN_MONO
                                                : SND_MIXER_SCHN_FRONT_LEFT;
    return snd_mixer_selem_get_playback_dB(elem_, ch, centi_db) == 0;
  }

  bool SetDbCenti(long centi_db) {
    if (!elem_) {
      return false;
    }
    if (use_capture_) {
      return snd_mixer_selem_set_capture_dB_all(elem_, centi_db, 0) == 0;
    }
    return snd_mixer_selem_set_playback_dB_all(elem_, centi_db, 0) == 0;
  }

  bool use_capture_ = false;
  snd_mixer_t* mixer_ = nullptr;
  snd_mixer_elem_t* elem_ = nullptr;
  long min_db_ = 0;
  long max_db_ = 0;
};

struct UpsBatteryStatus {
  bool valid = false;
  int percent = 0;
  double bus_voltage_v = 0.0;
};

class UpsHatBMonitor {
 public:
  ~UpsHatBMonitor() { Close(); }

  bool Open(int i2c_bus = 1, uint8_t addr = 0x42) {
#ifdef __linux__
    Close();
    char dev[32];
    std::snprintf(dev, sizeof(dev), "/dev/i2c-%d", i2c_bus);
    fd_ = open(dev, O_RDWR);
    if (fd_ < 0) {
      return false;
    }
    if (ioctl(fd_, I2C_SLAVE, addr) < 0) {
      Close();
      return false;
    }
    if (!WriteReg16(0x05, 4096)) {
      Close();
      return false;
    }
    return true;
#else
    (void)i2c_bus;
    (void)addr;
    return false;
#endif
  }

  void Close() {
#ifdef __linux__
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
#endif
  }

  bool IsOpen() const {
#ifdef __linux__
    return fd_ >= 0;
#else
    return false;
#endif
  }

  bool Read(UpsBatteryStatus* out) {
    if (!out || !IsOpen()) {
      return false;
    }
#ifdef __linux__
    uint16_t raw = 0;
    if (!WriteReg16(0x05, 4096) || !ReadReg16(0x02, &raw)) {
      return false;
    }
    const double bus_v = static_cast<double>((raw >> 3) & 0x1FFF) * 0.004;
    const double p = ((bus_v - 6.0) / 2.4) * 100.0;
    out->bus_voltage_v = bus_v;
    out->percent =
        static_cast<int>(std::max(0.0, std::min(100.0, p)));
    out->valid = true;
    return true;
#else
    return false;
#endif
  }

 private:
#ifdef __linux__
  bool WriteReg16(uint8_t reg, uint16_t value) {
    if (fd_ < 0) {
      return false;
    }
    uint8_t data[3];
    data[0] = reg;
    data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[2] = static_cast<uint8_t>(value & 0xFF);
    return write(fd_, data, 3) == 3;
  }

  bool ReadReg16(uint8_t reg, uint16_t* out) {
    if (fd_ < 0 || !out) {
      return false;
    }
    if (write(fd_, &reg, 1) != 1) {
      return false;
    }
    uint8_t data[2];
    if (read(fd_, data, 2) != 2) {
      return false;
    }
    *out = static_cast<uint16_t>((data[0] << 8) | data[1]);
    return true;
  }

  int fd_ = -1;
#endif
};

#ifdef __linux__

struct UiSnapshot {
  bool recording = false;
  bool monitoring = false;
  std::string mic;
  bool mic_connected = true;
  bool battery_valid = false;
  int battery_pct = 0;
  bool storage_valid = false;
  uint64_t remaining_storage_sec = 0;
  bool finalize_pending = false;
  unsigned int rate = 0;
  int channels = 0;
  bool zylia_gain_valid = false;
  int zylia_gain_db = 0;
  int peak_pct = 0;
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

  void PollButtons(bool* start, bool* stop, bool* mic_left, bool* mic_right,
                   bool* rate_up, bool* rate_down, bool repeat_ud = false) {
    if (start) *start = false;
    if (stop) *stop = false;
    if (mic_left) *mic_left = false;
    if (mic_right) *mic_right = false;
    if (rate_up) *rate_up = false;
    if (rate_down) *rate_down = false;
    const int key1 = ReadGpio(btn_[0]);
    const int key2 = ReadGpio(btn_[1]);
    const int key3 = ReadGpio(btn_[2]);
    const int joy_up = ReadGpio(btn_[3]);
    const int joy_down = ReadGpio(btn_[4]);
    const int joy_left = ReadGpio(btn_[5]);
    const int joy_right = ReadGpio(btn_[6]);
    auto edge = [&](int idx, int value) -> bool {
      return value >= 0 && last_btn_[idx] == idle_btn_[idx] &&
             value != idle_btn_[idx];
    };
    const auto now = std::chrono::steady_clock::now();
    auto pressed = [&](int idx, int value) -> bool {
      return value >= 0 && value != idle_btn_[idx];
    };
    auto was_pressed = [&](int idx) -> bool {
      return last_btn_[idx] != idle_btn_[idx];
    };
    auto hold_repeat = [&](int idx, int value, bool* out) {
      if (!out) {
        return;
      }
      if (pressed(idx, value)) {
        if (!was_pressed(idx)) {
          *out = true;
          next_repeat_[idx] = now + std::chrono::milliseconds(350);
        } else if (repeat_ud && now >= next_repeat_[idx]) {
          *out = true;
          next_repeat_[idx] = now + std::chrono::milliseconds(120);
        }
      } else {
        next_repeat_[idx] = std::chrono::steady_clock::time_point{};
      }
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
    if (mic_left && edge(5, joy_left)) {
      *mic_left = true;
    }
    if (mic_right && edge(6, joy_right)) {
      *mic_right = true;
    }
    hold_repeat(3, joy_up, rate_up);
    hold_repeat(4, joy_down, rate_down);
    if (key1 >= 0) last_btn_[0] = key1;
    if (key2 >= 0) last_btn_[1] = key2;
    if (key3 >= 0) last_btn_[2] = key3;
    if (joy_up >= 0) last_btn_[3] = joy_up;
    if (joy_down >= 0) last_btn_[4] = joy_down;
    if (joy_left >= 0) last_btn_[5] = joy_left;
    if (joy_right >= 0) last_btn_[6] = joy_right;
  }

  bool Render(const UiSnapshot& snap) {
    Clear(kBlack);
    const int margin = 12;
    const char* state = "IDLE";
    uint16_t state_dot = kDarkGray;
    if (snap.recording) {
      state = "REC";
      state_dot = kRed;
    } else if (snap.monitoring) {
      state = "MON";
      state_dot = kGreen;
    }
    FillRect(126, 6, 16, 16, state_dot);
    DrawText(146, 6, state, kWhite, 3);

    DrawText(margin, 22, "ELAP", kCyan, 2);
    DrawText(margin, 43, FormatHms(snap.elapsed_sec),
             snap.finalize_pending ? kRed : kYellow, 4);

    DrawText(margin, 86, "PEAK", kCyan, 2);
    const int meter_x = margin + 54;
    const int meter_y = 90;
    const int meter_w = 164;
    const int meter_h = 10;
    FillRect(meter_x - 1, meter_y - 1, meter_w + 2, meter_h + 2, kDarkGray);
    const int fill_w =
        (std::max(0, std::min(100, snap.peak_pct)) * meter_w) / 100;
    uint16_t meter_color = kGreen;
    if (snap.peak_pct >= 90) {
      meter_color = kRed;
    } else if (snap.peak_pct >= 70) {
      meter_color = kOrange;
    }
    if (fill_w > 0) {
      FillRect(meter_x, meter_y, fill_w, meter_h, meter_color);
    }
    if (fill_w < meter_w) {
      FillRect(meter_x + fill_w, meter_y, meter_w - fill_w, meter_h, kBlack);
    }

    DrawText(margin, 122, "MIC", kCyan, 2);
    DrawText(margin + 66, 122, snap.mic, snap.mic_connected ? kWhite : kRed, 2);

    char rate_ch[32];
    if (snap.zylia_gain_valid) {
      std::snprintf(rate_ch, sizeof(rate_ch), "%ukHz  CH:%d  G:%+ddB",
                    snap.rate / 1000, snap.channels, snap.zylia_gain_db);
    } else {
      std::snprintf(rate_ch, sizeof(rate_ch), "%ukHz  CH:%d", snap.rate / 1000,
                    snap.channels);
    }
    DrawText(margin, 146, rate_ch, kOrange, 2);

    char xr[32];
    std::snprintf(xr, sizeof(xr), "XRUN:%llu",
                  static_cast<unsigned long long>(snap.xruns));
    DrawText(margin, 176, xr, snap.xruns > 0 ? kRed : kWhite, 2);

    char dr[32];
    std::snprintf(dr, sizeof(dr), "DROP:%lluMB",
                  static_cast<unsigned long long>(snap.dropped_bytes /
                                                  (1024 * 1024)));
    DrawText(margin, 198, dr, snap.dropped_bytes > 0 ? kRed : kWhite, 2);

    char bat[16];
    if (snap.battery_valid) {
      std::snprintf(bat, sizeof(bat), "BAT %3d%%", snap.battery_pct);
    } else {
      std::snprintf(bat, sizeof(bat), "BAT --%%");
    }
    const int bat_scale = 2;
    const int bat_w = static_cast<int>(std::strlen(bat)) * 6 * bat_scale;
    const int bat_x = kWidth - margin - bat_w;
    const int bat_y = kHeight - (7 * bat_scale) - 6;
    uint16_t bat_color = kDarkGray;
    if (snap.battery_valid) {
      bat_color = (snap.battery_pct <= 20) ? kRed : kWhite;
    }
    DrawText(bat_x, bat_y, bat, bat_color, bat_scale);

    std::string rem = snap.storage_valid ? FormatHms(snap.remaining_storage_sec)
                                         : "--:--:--";
    uint16_t rem_color = kDarkGray;
    if (snap.storage_valid) {
      if (snap.remaining_storage_sec <= 600) {
        rem_color = kRed;
      } else if (snap.remaining_storage_sec <= 1800) {
        rem_color = kOrange;
      } else {
        rem_color = kGreen;
      }
    }
    DrawText(margin, bat_y, rem, rem_color, bat_scale);

    // DrawText(margin, 184, "KEY1:STOP KEY2:REC KEY3:BL", kDarkGray, 1);
    // DrawText(margin, 202, "L SPC R ZYL  U96 D48", kDarkGray, 1);
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
  static constexpr uint16_t kOrange = 0xFD20;

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
  std::chrono::steady_clock::time_point next_repeat_[8];
  bool backlight_on_ = true;
  std::string last_error_;
  std::vector<uint8_t> frame_ =
      std::vector<uint8_t>(static_cast<size_t>(kWidth * kHeight * 2), 0);
};

#else

struct UiSnapshot {
  bool recording = false;
  bool monitoring = false;
  std::string mic;
  bool mic_connected = true;
  bool battery_valid = false;
  int battery_pct = 0;
  bool storage_valid = false;
  uint64_t remaining_storage_sec = 0;
  bool finalize_pending = false;
  unsigned int rate = 0;
  int channels = 0;
  bool zylia_gain_valid = false;
  int zylia_gain_db = 0;
  int peak_pct = 0;
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
  void PollButtons(bool* start, bool* stop, bool* mic_left, bool* mic_right,
                   bool* rate_up, bool* rate_down, bool repeat_ud = false) {
    (void)repeat_ud;
    if (start) *start = false;
    if (stop) *stop = false;
    if (mic_left) *mic_left = false;
    if (mic_right) *mic_right = false;
    if (rate_up) *rate_up = false;
    if (rate_down) *rate_down = false;
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
    if (!opt.hat_ui && opt.mic == MicKind::kUnspecified) {
      std::fprintf(stderr,
                   "Auto filename requires --mic spcmic|zylia "
                   "(or provide --out)\n");
      return 1;
    }
    if (!opt.hat_ui) {
      opt.out_path = BuildAutoOutPath(opt.mic);
    }
  } else {
    opt.out_path = EnsureRecordingsPath(opt.out_path);
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  snd_pcm_t* pcm = nullptr;
  snd_pcm_uframes_t period_size = 0;
  snd_pcm_uframes_t buffer_size = 0;
  unsigned int actual_rate = static_cast<unsigned int>(opt.rate);
  MicKind selected_mic =
      (opt.mic == MicKind::kUnspecified) ? MicKind::kSpcmic : opt.mic;
  int spcmic_rate_hz = (opt.rate == 96000) ? 96000 : 48000;
  std::string current_device = opt.device;
  int current_channels = opt.channels;
  snd_pcm_access_t current_access = opt.access;
  auto ApplyMicPreset = [&](MicKind mic) {
    selected_mic = mic;
    if (mic == MicKind::kSpcmic) {
      current_device = "hw:CARD=s02E5D5,DEV=0";
      current_channels = 84;
      current_access = SND_PCM_ACCESS_RW_INTERLEAVED;
    } else if (mic == MicKind::kZylia) {
      current_device = "hw:CARD=ZM13E,DEV=0";
      current_channels = 19;
      current_access = SND_PCM_ACCESS_MMAP_INTERLEAVED;
    }
  };
  if (opt.hat_ui &&
      (selected_mic == MicKind::kSpcmic || selected_mic == MicKind::kZylia)) {
    ApplyMicPreset(selected_mic);
    if (selected_mic == MicKind::kZylia) {
      opt.rate = 48000;
      actual_rate = 48000;
    }
  }

  const int bytes_per_sample =
      (opt.format == SND_PCM_FORMAT_S16_LE) ? 2 : 3;
  int bytes_per_frame = 0;
  size_t period_bytes = 0;
  size_t ring_bytes = 0;
  std::vector<uint8_t> buffer;
  std::atomic<bool> mic_available{true};

  auto SetupCapture = [&]() -> bool {
    auto SetFallbackTiming = [&]() {
      const uint64_t frames =
          (static_cast<uint64_t>(opt.rate) * opt.period_ms) / 1000;
      period_size = static_cast<snd_pcm_uframes_t>(frames > 0 ? frames : 1);
      buffer_size = period_size * 4;
      actual_rate = static_cast<unsigned int>(opt.rate);
    };

    if (opt.stdin_raw) {
      SetFallbackTiming();
      return true;
    }

    if (pcm) {
      snd_pcm_close(pcm);
      pcm = nullptr;
    }

    int err =
        snd_pcm_open(&pcm, current_device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
      std::fprintf(stderr, "snd_pcm_open failed (%s): %s\n",
                   current_device.c_str(), snd_strerror(err));
      SetFallbackTiming();
      return false;
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

    if (!Check(snd_pcm_hw_params_set_access(pcm, params, current_access),
               "snd_pcm_hw_params_set_access") ||
        !Check(snd_pcm_hw_params_set_format(pcm, params, opt.format),
               "snd_pcm_hw_params_set_format") ||
        !Check(snd_pcm_hw_params_set_channels(pcm, params, current_channels),
               "snd_pcm_hw_params_set_channels")) {
      snd_pcm_hw_params_free(params);
      snd_pcm_close(pcm);
      pcm = nullptr;
      SetFallbackTiming();
      return false;
    }

    unsigned int rate = static_cast<unsigned int>(opt.rate);
    snd_pcm_hw_params_set_rate_resample(pcm, params, 0);
    if (!Check(snd_pcm_hw_params_set_rate_near(pcm, params, &rate, nullptr),
               "snd_pcm_hw_params_set_rate_near")) {
      snd_pcm_hw_params_free(params);
      snd_pcm_close(pcm);
      pcm = nullptr;
      SetFallbackTiming();
      return false;
    }

    unsigned int period_time = static_cast<unsigned int>(opt.period_ms) * 1000;
    if (!Check(snd_pcm_hw_params_set_period_time_near(
                   pcm, params, &period_time, nullptr),
               "snd_pcm_hw_params_set_period_time_near")) {
      snd_pcm_hw_params_free(params);
      snd_pcm_close(pcm);
      pcm = nullptr;
      SetFallbackTiming();
      return false;
    }

    unsigned int buffer_time = static_cast<unsigned int>(opt.buffer_ms) * 1000;
    if (!Check(snd_pcm_hw_params_set_buffer_time_near(
                   pcm, params, &buffer_time, nullptr),
               "snd_pcm_hw_params_set_buffer_time_near")) {
      snd_pcm_hw_params_free(params);
      snd_pcm_close(pcm);
      pcm = nullptr;
      SetFallbackTiming();
      return false;
    }

    err = snd_pcm_hw_params(pcm, params);
    if (err < 0) {
      std::fprintf(stderr, "snd_pcm_hw_params failed: %s\n", snd_strerror(err));
      snd_pcm_hw_params_free(params);
      snd_pcm_close(pcm);
      pcm = nullptr;
      SetFallbackTiming();
      return false;
    }

    snd_pcm_hw_params_get_period_size(params, &period_size, nullptr);
    snd_pcm_hw_params_get_buffer_size(params, &buffer_size);
    snd_pcm_hw_params_get_rate(params, &actual_rate, nullptr);
    snd_pcm_hw_params_free(params);

    snd_pcm_sw_params_t* swparams = nullptr;
    snd_pcm_sw_params_malloc(&swparams);
    snd_pcm_sw_params_current(pcm, swparams);
    snd_pcm_sw_params_set_start_threshold(pcm, swparams, 1);
    snd_pcm_sw_params_set_avail_min(pcm, swparams, period_size);
    err = snd_pcm_sw_params(pcm, swparams);
    snd_pcm_sw_params_free(swparams);
    if (err < 0) {
      std::fprintf(stderr, "snd_pcm_sw_params failed: %s\n", snd_strerror(err));
      snd_pcm_close(pcm);
      pcm = nullptr;
      SetFallbackTiming();
      return false;
    }

    if (actual_rate != static_cast<unsigned int>(opt.rate)) {
      std::fprintf(stderr, "Warning: requested rate %d, got %u\n", opt.rate,
                   actual_rate);
    }
    return true;
  };

  auto RefreshDerivedSizes = [&]() -> bool {
    if (period_size == 0) {
      std::fprintf(stderr, "Invalid period size\n");
      return false;
    }
    bytes_per_frame = current_channels * bytes_per_sample;
    period_bytes = static_cast<size_t>(period_size) * bytes_per_frame;
    buffer.resize(period_bytes);
    ring_bytes = static_cast<size_t>(actual_rate) * bytes_per_frame *
                 static_cast<size_t>(opt.ring_ms) / 1000;
    if (ring_bytes < period_bytes * 2) {
      ring_bytes = period_bytes * 2;
    }
    ring_bytes -= ring_bytes % bytes_per_frame;
    return true;
  };

  const bool initial_setup_ok = SetupCapture();
  mic_available.store(initial_setup_ok);
  if (!RefreshDerivedSizes()) {
    return 1;
  }
  if (!initial_setup_ok) {
    if (!opt.hat_ui) {
      if (pcm) {
        snd_pcm_close(pcm);
      }
      return 1;
    }
    std::fprintf(stdout,
                 "Selected mic is not connected. Mic label will be red and "
                 "recording is disabled until available.\n");
  }

  RingBuffer ring(ring_bytes);
  std::mutex ring_mutex;
  std::condition_variable ring_cv;

  SNDFILE* snd = nullptr;
  std::thread writer;
  std::atomic<bool> writer_running{false};
  std::atomic<bool> writer_error{false};

  std::atomic<uint64_t> dropped_bytes{0};
  std::atomic<uint64_t> xrun_count{0};
  std::atomic<bool> monitoring_active{false};
  std::atomic<bool> recording_active{false};
  std::atomic<bool> finalize_in_progress{false};
  std::atomic<bool> start_requested{false};
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> mic_left_requested{false};
  std::atomic<bool> mic_right_requested{false};
  std::atomic<bool> rate_up_requested{false};
  std::atomic<bool> rate_down_requested{false};
  std::atomic<int64_t> record_start_ms{0};
  std::atomic<uint64_t> finalize_elapsed_sec{0};
  std::atomic<int> peak_percent{0};
  std::atomic<bool> joy_ud_repeat{false};
  std::atomic<bool> zylia_gain_valid{false};
  std::atomic<int> zylia_gain_db{0};
  std::atomic<bool> battery_valid{false};
  std::atomic<int> battery_pct{0};
  std::atomic<bool> storage_valid{false};
  std::atomic<uint64_t> remaining_storage_sec{0};
  std::atomic<uint64_t> bytes_per_second{0};
  ZyliaGainControl zylia_gain_ctl;
  UpsHatBMonitor ups_hat_monitor;

  uint64_t take_count = 0;
  std::string current_out_path;

  const char* fmt_label =
      (opt.format == SND_PCM_FORMAT_S16_LE) ? "S16_LE" : "S24_3LE";
  const char* start_label = opt.explicit_start ? "explicit" : "auto";
  auto AccessLabel = [&]() -> const char* {
    return (current_access == SND_PCM_ACCESS_MMAP_INTERLEAVED) ? "MMAP" : "RW";
  };
  auto RefreshZyliaGain = [&]() {
    zylia_gain_valid.store(false);
    zylia_gain_db.store(0);
    if (selected_mic != MicKind::kZylia || !mic_available.load()) {
      zylia_gain_ctl.Close();
      return;
    }
    if (!zylia_gain_ctl.IsOpen() && !zylia_gain_ctl.Open("hw:CARD=ZM13E")) {
      return;
    }
    int db = 0;
    if (zylia_gain_ctl.ReadDb(&db)) {
      zylia_gain_db.store(db);
      zylia_gain_valid.store(true);
    }
  };
  auto RefreshBatteryStatus = [&]() {
    UpsBatteryStatus st;
    if (ups_hat_monitor.Read(&st)) {
      battery_valid.store(st.valid);
      battery_pct.store(st.percent);
    } else {
      battery_valid.store(false);
    }
  };
  auto PlannedOutputPath = [&]() -> std::string {
    if (!current_out_path.empty() &&
        (recording_active.load() || finalize_in_progress.load())) {
      return current_out_path;
    }
    if (!opt.hat_ui) {
      return opt.out_path;
    }
    if (opt.out_overridden) {
      return BuildManualTakePath(opt.out_path, static_cast<int>(take_count + 1));
    }
    return BuildAutoOutPath(selected_mic);
  };
  auto RefreshStorageRemaining = [&]() {
    const uint64_t bps = bytes_per_second.load();
    if (bps == 0) {
      storage_valid.store(false);
      return;
    }
    uint64_t free_bytes = 0;
    if (!GetFreeBytesForPath(PlannedOutputPath(), &free_bytes)) {
      storage_valid.store(false);
      return;
    }
    remaining_storage_sec.store(free_bytes / bps);
    storage_valid.store(true);
  };
  const std::string out_preview =
      opt.out_overridden
          ? opt.out_path
          : (std::string(kDefaultRecordingsDir) + "/" +
             MicKindToString(selected_mic) + "_YYYYMMDD_HHMMSS.rf64");
  RefreshZyliaGain();
  bytes_per_second.store(static_cast<uint64_t>(actual_rate) *
                         static_cast<uint64_t>(bytes_per_frame));
  RefreshStorageRemaining();
  if (opt.hat_ui) {
    if (ups_hat_monitor.Open()) {
      RefreshBatteryStatus();
    } else {
      std::fprintf(stdout,
                   "UPS HAT battery monitor unavailable "
                   "(check I2C and address 0x42).\n");
    }
  }

  if (opt.hat_ui) {
    std::fprintf(stdout,
                 "Idle mode enabled on HAT UI.\n"
                 "ALSA device: %s | period: %lu frames | buffer: %lu frames\n"
                 "Format: %d ch @ %u Hz (%s) | access: %s | start: %s\n"
                  "Output file: %s\n"
                  "Ring buffer: %lu ms (~%lu MB)\n"
                  "Press KEY2 for MON, KEY2 again for REC, KEY1 to stop, Ctrl+C to exit.\n",
                 current_device.c_str(), static_cast<unsigned long>(period_size),
                 static_cast<unsigned long>(buffer_size), current_channels,
                 actual_rate, fmt_label, AccessLabel(), start_label,
                 out_preview.c_str(), static_cast<unsigned long>(opt.ring_ms),
                 static_cast<unsigned long>(ring_bytes / (1024 * 1024)));
  } else if (opt.stdin_raw) {
    std::fprintf(stdout,
                 "Recording %d ch @ %u Hz (%s) to %s\n"
                 "Input: stdin raw PCM | period: %lu frames\n"
                 "Ring buffer: %lu ms (~%lu MB)\n"
                 "Press Ctrl+C to stop...\n",
                 current_channels, actual_rate, fmt_label, opt.out_path.c_str(),
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
                 current_channels, actual_rate, fmt_label, opt.out_path.c_str(),
                 current_device.c_str(), static_cast<unsigned long>(period_size),
                 static_cast<unsigned long>(buffer_size), AccessLabel(),
                 start_label, static_cast<unsigned long>(opt.ring_ms),
                 static_cast<unsigned long>(ring_bytes / (1024 * 1024)));
  }

  auto next_status = std::chrono::steady_clock::now();
  const auto status_interval = std::chrono::milliseconds(opt.status_ms);
  auto next_battery_poll = std::chrono::steady_clock::now();
  const auto battery_interval = std::chrono::seconds(10);
  auto next_storage_poll = std::chrono::steady_clock::now();
  const auto storage_interval = std::chrono::seconds(2);
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

  auto start_monitoring = [&]() -> bool {
    if (monitoring_active.load() || recording_active.load()) {
      return true;
    }
    if (!opt.stdin_raw) {
      int err = snd_pcm_prepare(pcm);
      if (err < 0) {
        std::fprintf(stderr, "snd_pcm_prepare failed: %s\n", snd_strerror(err));
        return false;
      }
      if (opt.explicit_start) {
        err = snd_pcm_start(pcm);
        if (err < 0) {
          std::fprintf(stderr, "snd_pcm_start failed: %s\n", snd_strerror(err));
          return false;
        }
      }
    }
    monitoring_active.store(true);
    peak_percent.store(0);
    record_start_ms.store(0);
    next_status = std::chrono::steady_clock::now();
    std::fprintf(stdout, "Monitoring started\n");
    return true;
  };

  auto stop_monitoring = [&]() {
    if (!monitoring_active.load()) {
      return;
    }
    if (!opt.stdin_raw && pcm) {
      snd_pcm_drop(pcm);
    }
    monitoring_active.store(false);
    peak_percent.store(0);
    std::fprintf(stdout, "Monitoring stopped\n");
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
                             : BuildAutoOutPath(selected_mic);
    }

    {
      std::lock_guard<std::mutex> lock(ring_mutex);
      ring.Clear();
    }
    dropped_bytes.store(0);
    xrun_count.store(0);

    std::string mkdir_error;
    if (!EnsureParentDirectoryExists(current_out_path, &mkdir_error)) {
      std::fprintf(stderr, "Failed to create output directory for %s: %s\n",
                   current_out_path.c_str(), mkdir_error.c_str());
      return false;
    }

    SF_INFO info{};
    info.samplerate = static_cast<int>(actual_rate);
    info.channels = current_channels;
    info.format = SF_FORMAT_RF64 |
                  ((opt.format == SND_PCM_FORMAT_S16_LE) ? SF_FORMAT_PCM_16
                                                         : SF_FORMAT_PCM_24);
    snd = sf_open(current_out_path.c_str(), SFM_WRITE, &info);
    if (!snd) {
      std::fprintf(stderr, "sf_open failed for %s: %s\n",
                   current_out_path.c_str(), sf_strerror(nullptr));
      return false;
    }
    sf_command(snd, SFC_RF64_AUTO_DOWNGRADE, nullptr, SF_TRUE);

    if (!opt.stdin_raw && !monitoring_active.load()) {
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
    monitoring_active.store(false);
    finalize_in_progress.store(false);
    finalize_elapsed_sec.store(0);
    peak_percent.store(0);
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

    const int64_t start_ms = record_start_ms.load();
    if (start_ms > 0) {
      const int64_t delta_ms = std::max<int64_t>(0, now_ms() - start_ms);
      finalize_elapsed_sec.store(static_cast<uint64_t>(delta_ms / 1000));
    } else {
      finalize_elapsed_sec.store(0);
    }
    finalize_in_progress.store(true);
    if (!opt.stdin_raw && pcm) {
      snd_pcm_drop(pcm);
    }

    recording_active.store(false);
    monitoring_active.store(false);
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
    record_start_ms.store(0);
    peak_percent.store(0);
    finalize_elapsed_sec.store(0);
    finalize_in_progress.store(false);
  };

  auto make_ui_snapshot = [&]() -> UiSnapshot {
    UiSnapshot snap;
    snap.recording = recording_active.load();
    snap.monitoring = monitoring_active.load();
    snap.finalize_pending = finalize_in_progress.load();
    snap.mic = MicKindToString(selected_mic);
    snap.mic_connected = mic_available.load();
    snap.battery_valid = battery_valid.load();
    snap.battery_pct = battery_pct.load();
    snap.storage_valid = storage_valid.load();
    snap.remaining_storage_sec = remaining_storage_sec.load();
    snap.rate = actual_rate;
    snap.channels = current_channels;
    snap.zylia_gain_valid = zylia_gain_valid.load();
    snap.zylia_gain_db = zylia_gain_db.load();
    snap.peak_pct = peak_percent.load();
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
    } else if (snap.monitoring) {
      snap.elapsed_sec = 0;
    } else if (snap.finalize_pending) {
      snap.elapsed_sec = finalize_elapsed_sec.load();
      snap.peak_pct = 0;
    } else {
      snap.peak_pct = 0;
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
        bool mic_left_evt = false;
        bool mic_right_evt = false;
        bool rate_up_evt = false;
        bool rate_down_evt = false;
        hat_ui->PollButtons(&start_evt, &stop_evt, &mic_left_evt,
                            &mic_right_evt, &rate_up_evt, &rate_down_evt,
                            joy_ud_repeat.load());
        if (start_evt && !finalize_in_progress.load()) {
          start_requested.store(true);
        }
        if (stop_evt) {
          stop_requested.store(true);
        }
        if (mic_left_evt && !recording_active.load() && !monitoring_active.load()) {
          mic_left_requested.store(true);
        }
        if (mic_right_evt && !recording_active.load() && !monitoring_active.load()) {
          mic_right_requested.store(true);
        }
        if (rate_up_evt) {
          rate_up_requested.store(true);
        }
        if (rate_down_evt) {
          rate_down_requested.store(true);
        }
        hat_ui->Render(make_ui_snapshot());
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
      }
      UiSnapshot snap = make_ui_snapshot();
      snap.recording = false;
      snap.monitoring = false;
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
    if (opt.hat_ui) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_battery_poll) {
        if (ups_hat_monitor.IsOpen()) {
          RefreshBatteryStatus();
        }
        next_battery_poll = now + battery_interval;
      }
      if (now >= next_storage_poll) {
        RefreshStorageRemaining();
        next_storage_poll = now + storage_interval;
      }
    }

    const bool is_rec = recording_active.load();
    const bool is_mon = monitoring_active.load();
    joy_ud_repeat.store(selected_mic == MicKind::kZylia);

    if (opt.hat_ui && !is_rec && !is_mon) {
      bool mic_changed = false;
      bool rate_changed = false;
      int gain_delta = 0;

      if (mic_left_requested.exchange(false) &&
          selected_mic != MicKind::kSpcmic) {
        ApplyMicPreset(MicKind::kSpcmic);
        mic_changed = true;
        if (opt.rate != spcmic_rate_hz) {
          opt.rate = spcmic_rate_hz;
          rate_changed = true;
        }
      }
      if (mic_right_requested.exchange(false) &&
          selected_mic != MicKind::kZylia) {
        if (selected_mic == MicKind::kSpcmic) {
          spcmic_rate_hz = opt.rate;
        }
        ApplyMicPreset(MicKind::kZylia);
        mic_changed = true;
        if (opt.rate != 48000) {
          opt.rate = 48000;
          rate_changed = true;
        }
      }

      const bool up_evt = rate_up_requested.exchange(false);
      const bool down_evt = rate_down_requested.exchange(false);
      if (selected_mic == MicKind::kSpcmic) {
        if (up_evt && opt.rate != 96000) {
          opt.rate = 96000;
          spcmic_rate_hz = opt.rate;
          rate_changed = true;
        }
        if (down_evt && opt.rate != 48000) {
          opt.rate = 48000;
          spcmic_rate_hz = opt.rate;
          rate_changed = true;
        }
      } else if (selected_mic == MicKind::kZylia) {
        if (up_evt) {
          gain_delta += 1;
        }
        if (down_evt) {
          gain_delta -= 1;
        }
      }

      if (mic_changed || rate_changed) {
        const bool setup_ok = SetupCapture();
        mic_available.store(setup_ok);
        if (!RefreshDerivedSizes()) {
          std::fprintf(stderr, "Failed to update capture sizes in idle mode\n");
          g_running.store(false);
          break;
        }
        bytes_per_second.store(static_cast<uint64_t>(actual_rate) *
                               static_cast<uint64_t>(bytes_per_frame));
        RefreshStorageRemaining();
        RefreshZyliaGain();
        {
          std::lock_guard<std::mutex> lock(ring_mutex);
          ring = RingBuffer(ring_bytes);
        }
        if (setup_ok) {
          std::fprintf(stdout, "Selected: %s | %d Hz | %d ch | %s\n",
                       MicKindToString(selected_mic), opt.rate,
                       current_channels, AccessLabel());
        } else {
          std::fprintf(stdout,
                       "Selected: %s | %d Hz (mic not connected, recording "
                       "disabled)\n",
                       MicKindToString(selected_mic), opt.rate);
        }
      }
      if (selected_mic == MicKind::kZylia && gain_delta != 0 &&
          mic_available.load() && zylia_gain_ctl.StepDb(gain_delta)) {
        RefreshZyliaGain();
      }

      if (start_requested.exchange(false)) {
        if (!mic_available.load()) {
          std::fprintf(stdout,
                       "Cannot start: selected mic is not connected.\n");
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          continue;
        }
        if (!start_monitoring()) {
          std::fprintf(stderr, "Failed to start monitoring\n");
          mic_available.store(false);
        }
      }
      stop_requested.store(false);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    if (opt.hat_ui) {
      if (is_mon && stop_requested.exchange(false)) {
        stop_monitoring();
        continue;
      }
      if (is_rec && stop_requested.exchange(false)) {
        stop_take();
        continue;
      }
      if (start_requested.exchange(false)) {
        if (is_mon) {
          if (!start_take()) {
            std::fprintf(stderr, "Failed to start take from MON\n");
          }
        } else if (!is_rec && !finalize_in_progress.load()) {
          if (!mic_available.load()) {
            std::fprintf(stdout, "Cannot monitor: selected mic is not connected.\n");
          } else if (!start_monitoring()) {
            std::fprintf(stderr, "Failed to start monitoring\n");
            mic_available.store(false);
          }
        }
      }

      const bool up_evt = rate_up_requested.exchange(false);
      const bool down_evt = rate_down_requested.exchange(false);
      if ((is_mon || is_rec) && selected_mic == MicKind::kZylia &&
          mic_available.load()) {
        int gain_delta = 0;
        if (up_evt) {
          gain_delta += 1;
        }
        if (down_evt) {
          gain_delta -= 1;
        }
        if (gain_delta != 0 && zylia_gain_ctl.StepDb(gain_delta)) {
          RefreshZyliaGain();
        }
      }
    }

    if (!recording_active.load() && !monitoring_active.load()) {
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
      peak_percent.store(
          std::max(0, std::min(100, ComputePeakPercent(buffer.data(), bytes,
                                                       opt.format))));
      if (recording_active.load()) {
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
    } else {
      snd_pcm_sframes_t frames =
          (current_access == SND_PCM_ACCESS_MMAP_INTERLEAVED)
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
      peak_percent.store(
          std::max(0, std::min(100, ComputePeakPercent(buffer.data(), bytes,
                                                       opt.format))));
      if (recording_active.load()) {
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
  if (monitoring_active.load()) {
    stop_monitoring();
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
  zylia_gain_ctl.Close();
  ups_hat_monitor.Close();
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

#include <alsa/asoundlib.h>
#include <sndfile.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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
      "  Auto naming requires --mic spcmic|zylia.\n",
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

 private:
  std::vector<uint8_t> buf_;
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t size_ = 0;
};

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

  if (!opt.out_overridden) {
    if (opt.mic == MicKind::kUnspecified) {
      std::fprintf(stderr,
                   "Auto filename requires --mic spcmic|zylia "
                   "(or provide --out)\n");
      return 1;
    }
    opt.out_path = BuildAutoOutPath(opt.mic);
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
    snd_pcm_close(pcm);
    return 1;
  }

  std::vector<uint8_t> buffer;
  buffer.resize(static_cast<size_t>(period_size) * bytes_per_frame);

  SF_INFO info{};
  info.samplerate = static_cast<int>(actual_rate);
  info.channels = opt.channels;
  info.format = SF_FORMAT_RF64 |
                ((opt.format == SND_PCM_FORMAT_S16_LE) ? SF_FORMAT_PCM_16
                                                       : SF_FORMAT_PCM_24);

  SNDFILE* snd = sf_open(opt.out_path.c_str(), SFM_WRITE, &info);
  if (!snd) {
    std::fprintf(stderr, "sf_open failed: %s\n", sf_strerror(nullptr));
    snd_pcm_close(pcm);
    return 1;
  }

  // Allow auto downgrade to WAV if file is < 4GB on close.
  sf_command(snd, SFC_RF64_AUTO_DOWNGRADE, nullptr, SF_TRUE);

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

  std::atomic<bool> writer_error{false};
  std::atomic<uint64_t> dropped_bytes{0};
  std::atomic<uint64_t> xrun_count{0};

  std::thread writer([&] {
    std::vector<uint8_t> out;
    out.resize(period_bytes);
    while (g_running.load() || ring.size() > 0) {
      size_t bytes = 0;
      {
        std::unique_lock<std::mutex> lock(ring_mutex);
        ring_cv.wait_for(lock, std::chrono::milliseconds(200), [&] {
          return ring.size() > 0 || !g_running.load();
        });
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
        g_running.store(false);
        break;
      }
    }
  });

  const char* fmt_label =
      (opt.format == SND_PCM_FORMAT_S16_LE) ? "S16_LE" : "S24_3LE";
  const char* access_label =
      (opt.access == SND_PCM_ACCESS_MMAP_INTERLEAVED) ? "MMAP" : "RW";
  const char* start_label = opt.explicit_start ? "explicit" : "auto";
  if (opt.stdin_raw) {
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
    int err = snd_pcm_sw_params(pcm, swparams);
    snd_pcm_sw_params_free(swparams);
    if (err < 0) {
      std::fprintf(stderr, "snd_pcm_sw_params failed: %s\n",
                   snd_strerror(err));
      snd_pcm_close(pcm);
      sf_close(snd);
      return 1;
    }

    err = snd_pcm_prepare(pcm);
    if (err < 0) {
      std::fprintf(stderr, "snd_pcm_prepare failed: %s\n", snd_strerror(err));
      snd_pcm_close(pcm);
      sf_close(snd);
      return 1;
    }
    if (opt.explicit_start) {
      err = snd_pcm_start(pcm);
      if (err < 0) {
        std::fprintf(stderr, "snd_pcm_start failed: %s\n", snd_strerror(err));
        snd_pcm_close(pcm);
        sf_close(snd);
        return 1;
      }
    }
  }

  auto next_status = std::chrono::steady_clock::now();
  const auto status_interval = std::chrono::milliseconds(opt.status_ms);

  while (g_running.load()) {
    if (opt.stdin_raw) {
      const size_t want = period_bytes;
      size_t got = std::fread(buffer.data(), 1, want, stdin);
      if (got == 0) {
        if (std::ferror(stdin)) {
          std::perror("stdin read");
        }
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

  g_running.store(false);
  ring_cv.notify_all();
  if (writer.joinable()) {
    writer.join();
  }

  sf_write_sync(snd);
  sf_close(snd);
  if (pcm) {
    snd_pcm_close(pcm);
  }
  std::fprintf(stdout, "Stopped. XRUNs: %llu | Dropped: %llu bytes\n",
               static_cast<unsigned long long>(xrun_count.load()),
               static_cast<unsigned long long>(dropped_bytes.load()));
  if (writer_error.load()) {
    return 1;
  }
  return 0;
}

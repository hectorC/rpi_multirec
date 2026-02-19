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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
  std::string device = "default";
  std::string out_path = "capture.rf64";
  int rate = 48000;
  int channels = 84;
  int buffer_ms = 200;
  int period_ms = 50;
  int ring_ms = 2000;
  bool list_devices = false;
  bool show_help = false;
};

std::atomic<bool> g_running{true};

void HandleSignal(int) {
  g_running.store(false);
}

void PrintUsage(const char* exe) {
  std::printf(
      "Usage: %s [options]\n"
      "\n"
      "Minimal multichannel recorder using ALSA + RF64 WAV.\n"
      "\n"
      "Options:\n"
      "  -d, --device <name>     ALSA device (default: \"default\")\n"
      "  -o, --out <path>        Output RF64 file (default: capture.rf64)\n"
      "  -r, --rate <48000|96000> Sample rate (default: 48000)\n"
      "  -c, --channels <n>      Channel count (default: 84)\n"
      "  --buffer-ms <ms>        ALSA buffer time in ms (default: 200)\n"
      "  --period-ms <ms>        ALSA period time in ms (default: 50)\n"
      "  --ring-ms <ms>          Ring buffer time in ms (default: 2000)\n"
      "  -L, --list-devices      List ALSA PCM devices and exit\n"
      "  -h, --help              Show this help\n",
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
      continue;
    }
    if (arg == "-o" || arg == "--out") {
      const char* v = need_value(arg.c_str());
      if (!v) return false;
      out->out_path = v;
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

    std::fprintf(stderr, "Unknown arg: %s\n", arg.c_str());
    PrintUsage(argv[0]);
    return false;
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

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  snd_pcm_t* pcm = nullptr;
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

  if (!Check(snd_pcm_hw_params_set_access(pcm, params,
                                         SND_PCM_ACCESS_RW_INTERLEAVED),
             "snd_pcm_hw_params_set_access") ||
      !Check(snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S24_3LE),
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
  if (!Check(snd_pcm_hw_params_set_period_time_near(pcm, params, &period_time,
                                                    nullptr),
             "snd_pcm_hw_params_set_period_time_near")) {
    snd_pcm_hw_params_free(params);
    snd_pcm_close(pcm);
    return 1;
  }

  unsigned int buffer_time = static_cast<unsigned int>(opt.buffer_ms) * 1000;
  if (!Check(snd_pcm_hw_params_set_buffer_time_near(pcm, params, &buffer_time,
                                                    nullptr),
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

  snd_pcm_uframes_t period_size = 0;
  snd_pcm_hw_params_get_period_size(params, &period_size, nullptr);

  snd_pcm_uframes_t buffer_size = 0;
  snd_pcm_hw_params_get_buffer_size(params, &buffer_size);

  unsigned int actual_rate = 0;
  snd_pcm_hw_params_get_rate(params, &actual_rate, nullptr);

  snd_pcm_hw_params_free(params);

  if (actual_rate != static_cast<unsigned int>(opt.rate)) {
    std::fprintf(stderr, "Warning: requested rate %d, got %u\n", opt.rate,
                 actual_rate);
  }

  const int bytes_per_sample = 3;  // S24_3LE
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
  info.format = SF_FORMAT_RF64 | SF_FORMAT_PCM_24;

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

  std::fprintf(stdout,
               "Recording %d ch @ %u Hz to %s\n"
               "ALSA device: %s | period: %lu frames | buffer: %lu frames\n"
               "Ring buffer: %lu ms (~%lu MB)\n"
               "Press Ctrl+C to stop...\n",
               opt.channels, actual_rate, opt.out_path.c_str(),
               opt.device.c_str(), static_cast<unsigned long>(period_size),
               static_cast<unsigned long>(buffer_size),
               static_cast<unsigned long>(opt.ring_ms),
               static_cast<unsigned long>(ring_bytes / (1024 * 1024)));

  while (g_running.load()) {
    snd_pcm_sframes_t frames =
        snd_pcm_readi(pcm, buffer.data(), period_size);
    if (frames == -EPIPE) {
      xrun_count.fetch_add(1);
      std::fprintf(stderr, "XRUN (overrun) detected\n");
      snd_pcm_prepare(pcm);
      continue;
    }
    if (frames < 0) {
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

  g_running.store(false);
  ring_cv.notify_all();
  if (writer.joinable()) {
    writer.join();
  }

  sf_write_sync(snd);
  sf_close(snd);
  snd_pcm_close(pcm);
  std::fprintf(stdout, "Stopped. XRUNs: %llu | Dropped: %llu bytes\n",
               static_cast<unsigned long long>(xrun_count.load()),
               static_cast<unsigned long long>(dropped_bytes.load()));
  if (writer_error.load()) {
    return 1;
  }
  return 0;
}

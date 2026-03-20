#include "app.h"

#include "alsa_utils.h"
#include "audio_utils.h"
#include "common.h"
#include "hat_ui.h"
#include "options.h"
#include "path_utils.h"
#include "types.h"
#include "ups_hat.h"
#include "zylia_gain.h"

namespace {

std::atomic<bool> g_running{true};

void HandleSignal(int) {
  g_running.store(false);
}

}  // namespace

int RunApp(int argc, char** argv) {
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

  const std::string external_recordings_dir = DetectExternalRecordingsDir();
  const bool use_external_storage = !external_recordings_dir.empty();
  SetRecordingsDir(use_external_storage ? external_recordings_dir
                                        : kDefaultRecordingsDir);

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
  std::string zylia_mixer_card = MixerCardFromPcmDevice(current_device);
  auto ApplyMicPreset = [&](MicKind mic) {
    selected_mic = mic;
    if (mic == MicKind::kSpcmic) {
      const AlsaDeviceMatch match =
          FindPreferredCaptureDevice({"spacemic"}, "hw:CARD=s02E5D5,DEV=0");
      current_device = match.hw_device;
      current_channels = 84;
      current_access = SND_PCM_ACCESS_RW_INTERLEAVED;
    } else if (mic == MicKind::kZylia) {
      const AlsaDeviceMatch match =
          FindPreferredCaptureDevice({"zylia", "zm-1", "zm1"},
                                     "hw:CARD=ZM13E,DEV=0",
                                     "hw:CARD=ZM13E");
      current_device = match.hw_device;
      zylia_mixer_card = match.mixer_card;
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
  std::atomic<bool> poweroff_requested{false};
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
  std::atomic<bool> using_external_storage{use_external_storage};
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
    if (!zylia_gain_ctl.IsOpen() &&
        !zylia_gain_ctl.Open(zylia_mixer_card.empty() ? "hw:CARD=ZM13E"
                                                      : zylia_mixer_card)) {
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
          : (RecordingsDir() + "/" +
             MicKindToString(selected_mic) + "_YYYYMMDD_HHMMSS.rf64");
  RefreshZyliaGain();
  bytes_per_second.store(static_cast<uint64_t>(actual_rate) *
                         static_cast<uint64_t>(bytes_per_frame));
  RefreshStorageRemaining();
  if (opt.hat_ui) {
    std::fprintf(stdout, "Recording root: %s%s\n", RecordingsDir().c_str(),
                 use_external_storage ? " (external)" : " (internal)");
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
  const auto storage_interval = std::chrono::seconds(30);
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
    snap.external_storage = using_external_storage.load();
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
  bool poweroff_after_shutdown = false;
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
        bool backlight_toggle_evt = false;
        bool poweroff_evt = false;
        hat_ui->PollButtons(&start_evt, &stop_evt, &mic_left_evt,
                            &mic_right_evt, &rate_up_evt, &rate_down_evt,
                            &backlight_toggle_evt, &poweroff_evt,
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
        if (backlight_toggle_evt) {
          hat_ui->ToggleBacklight();
        }
        if (poweroff_evt) {
          poweroff_requested.store(true);
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

#ifdef __linux__
  {
    std::string rt_error;
    int applied_priority = 0;
    if (ElevateCurrentThreadForCapture(20, &rt_error, &applied_priority)) {
      std::fprintf(stdout,
                   "Capture thread scheduling: SCHED_FIFO priority %d\n",
                   applied_priority);
    } else {
      std::fprintf(stdout,
                   "Warning: failed to enable realtime capture scheduling: %s\n",
                   rt_error.c_str());
    }
  }
#endif

  while (g_running.load()) {
    if (poweroff_requested.exchange(false)) {
      poweroff_after_shutdown = true;
      g_running.store(false);
      continue;
    }

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
  if (poweroff_after_shutdown && hat_ui) {
    hat_ui->ShowPoweroffMessage();
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
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
  if (poweroff_after_shutdown) {
#ifdef __linux__
    std::fprintf(stdout, "Powering off...\n");
    const int rc = std::system("systemctl poweroff");
    if (rc != 0) {
      std::fprintf(stderr, "systemctl poweroff failed with exit code %d\n", rc);
      return 1;
    }
    return 0;
#else
    std::fprintf(stderr, "Poweroff is only supported on Linux.\n");
    return 1;
#endif
  }
  if (writer_error.load()) {
    return 1;
  }
  return 0;
}

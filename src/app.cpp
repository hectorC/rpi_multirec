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

enum class UiMode {
  kSpcmic,
  kZylia,
  kPlayback,
  kSettings,
};

constexpr int kSpcmicPlaybackLeftChannel = 24;   // ch 25
constexpr int kSpcmicPlaybackRightChannel = 52;  // ch 53
constexpr int kZyliaPlaybackLeftChannel = 4;   // ch 5
constexpr int kZyliaPlaybackRightChannel = 7;  // ch 8
constexpr size_t kPlaybackVisibleItems = 6;

void HandleSignal(int) {
  g_running.store(false);
}

UiMode NextUiMode(UiMode mode) {
  switch (mode) {
    case UiMode::kSpcmic:
      return UiMode::kZylia;
    case UiMode::kZylia:
      return UiMode::kPlayback;
    case UiMode::kPlayback:
      return UiMode::kSettings;
    case UiMode::kSettings:
      return UiMode::kSpcmic;
  }
  return UiMode::kSpcmic;
}

UiMode PrevUiMode(UiMode mode) {
  switch (mode) {
    case UiMode::kSpcmic:
      return UiMode::kSettings;
    case UiMode::kZylia:
      return UiMode::kSpcmic;
    case UiMode::kPlayback:
      return UiMode::kZylia;
    case UiMode::kSettings:
      return UiMode::kPlayback;
  }
  return UiMode::kSpcmic;
}

std::vector<std::string> ListPlaybackFiles() {
  std::vector<std::string> files;
  std::error_code ec;
  const std::filesystem::path root(RecordingsDir());
  if (!std::filesystem::exists(root, ec) ||
      !std::filesystem::is_directory(root, ec)) {
    return files;
  }
  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec) || ec) {
      continue;
    }
    const std::string ext = ToLowerCopy(entry.path().extension().string());
    if (ext == ".rf64") {
      files.push_back(entry.path().string());
    }
  }
  std::sort(files.begin(), files.end(), [](const std::string& a,
                                           const std::string& b) {
    std::error_code ec_a;
    std::error_code ec_b;
    const auto ta = std::filesystem::last_write_time(a, ec_a);
    const auto tb = std::filesystem::last_write_time(b, ec_b);
    if (!ec_a && !ec_b && ta != tb) {
      return ta > tb;
    }
    return std::filesystem::path(a).filename().string() >
           std::filesystem::path(b).filename().string();
  });
  return files;
}

std::string MakePlaybackDisplayName(const std::string& file_path) {
  std::string stem = std::filesystem::path(file_path).stem().string();
  if (stem.size() > 18) {
    stem.resize(18);
  }
  return stem;
}

bool DeterminePlaybackMapping(const std::string& file_path, int file_channels,
                              int* left_idx, int* right_idx,
                              std::string* route_label) {
  const std::string lower_name =
      ToLowerCopy(std::filesystem::path(file_path).filename().string());
  if (((lower_name.rfind("spcmic_", 0) == 0 || lower_name.rfind("spc_", 0) == 0) ||
       file_channels == 84) &&
      file_channels > kSpcmicPlaybackRightChannel) {
    if (left_idx) *left_idx = kSpcmicPlaybackLeftChannel;
    if (right_idx) *right_idx = kSpcmicPlaybackRightChannel;
    if (route_label) *route_label = "SPCMIC CH25-53";
    return true;
  }
  if (lower_name.rfind("zylia_", 0) == 0 || lower_name.rfind("zyl_", 0) == 0 ||
      file_channels == 19) {
    if (left_idx) *left_idx = kZyliaPlaybackLeftChannel;
    if (right_idx) *right_idx = kZyliaPlaybackRightChannel;
    if (route_label) *route_label = "ZYLIA CH5-8";
    return file_channels > kZyliaPlaybackRightChannel;
  }
  if (file_channels == 1) {
    if (left_idx) *left_idx = 0;
    if (right_idx) *right_idx = 0;
    if (route_label) *route_label = "MONO";
    return true;
  }
  if (file_channels >= 2) {
    if (left_idx) *left_idx = 0;
    if (right_idx) *right_idx = 1;
    if (route_label) *route_label = "CH1-2";
    return true;
  }
  if (route_label) *route_label = "UNSUPPORTED";
  return false;
}

struct ManualClockState {
  int year = 2026;
  int month = 1;
  int day = 1;
  int hour = 0;
  int minute = 0;
  int second = 0;
};

bool IsLeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int DaysInMonth(int year, int month) {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 31;
  }
  if (month == 2 && IsLeapYear(year)) {
    return 29;
  }
  return kDays[month - 1];
}

void ClampManualClockState(ManualClockState* state) {
  if (!state) {
    return;
  }
  state->year = std::clamp(state->year, 2000, 2099);
  state->month = std::clamp(state->month, 1, 12);
  state->day = std::clamp(state->day, 1, DaysInMonth(state->year, state->month));
  state->hour = std::clamp(state->hour, 0, 23);
  state->minute = std::clamp(state->minute, 0, 59);
  state->second = std::clamp(state->second, 0, 59);
}

std::string FormatManualClockDate(const ManualClockState& state) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", state.year, state.month,
                state.day);
  return buf;
}

std::string FormatManualClockTime(const ManualClockState& state) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", state.hour, state.minute,
                state.second);
  return buf;
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
  uint64_t next_take_number = FindHighestExistingTakeNumber() + 1;

  if (!opt.out_overridden) {
    if (!opt.hat_ui && opt.mic == MicKind::kUnspecified) {
      std::fprintf(stderr,
                   "Auto filename requires --mic spcmic|zylia "
                   "(or provide --out)\n");
      return 1;
    }
    if (!opt.hat_ui) {
      opt.out_path = BuildAutoOutPath(opt.mic, false, next_take_number);
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
  UiMode current_ui_mode =
      (selected_mic == MicKind::kZylia) ? UiMode::kZylia : UiMode::kSpcmic;
  int spcmic_rate_hz = (opt.rate == 96000) ? 96000 : 48000;
  bool manual_time_set = false;
  ManualClockState manual_clock;
  int manual_clock_field = 0;
  bool settings_editing = false;
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
  std::atomic<bool> playback_active{false};
  std::atomic<bool> playback_stop_requested{false};
  std::atomic<int> playback_gain_db{0};
  std::atomic<uint64_t> playback_elapsed_sec{0};
  std::atomic<uint64_t> playback_selected_duration_sec{0};
  std::atomic<int64_t> playback_seek_seconds_pending{0};
  std::atomic<int64_t> record_start_ms{0};
  std::atomic<uint64_t> finalize_elapsed_sec{0};
  std::atomic<int> peak_percent{0};
  std::atomic<bool> joy_ud_repeat{false};
  std::atomic<bool> joy_lr_repeat{false};
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
  std::mutex playback_mutex;
  std::vector<std::string> playback_files;
  std::string playback_info = "NO FILES";
  bool playback_info_error = true;
  int playback_selected = 0;
  std::thread playback_thread;

  uint64_t take_count = 0;
  std::string current_out_path;

  auto LoadManualClockFromSystem = [&]() {
    std::time_t now = std::time(nullptr);
    std::tm tm_now{};
#ifdef _WIN32
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    manual_clock.year = tm_now.tm_year + 1900;
    manual_clock.month = tm_now.tm_mon + 1;
    manual_clock.day = tm_now.tm_mday;
    manual_clock.hour = tm_now.tm_hour;
    manual_clock.minute = tm_now.tm_min;
    manual_clock.second = tm_now.tm_sec;
    ClampManualClockState(&manual_clock);
  };

  auto AdjustManualClockField = [&](int field, int delta) {
    switch (field) {
      case 0:
        manual_clock.year = std::clamp(manual_clock.year + delta, 2000, 2099);
        break;
      case 1:
        manual_clock.month += delta;
        if (manual_clock.month < 1) manual_clock.month = 12;
        if (manual_clock.month > 12) manual_clock.month = 1;
        break;
      case 2:
        manual_clock.day += delta;
        if (manual_clock.day < 1) {
          manual_clock.day = DaysInMonth(manual_clock.year, manual_clock.month);
        }
        if (manual_clock.day > DaysInMonth(manual_clock.year, manual_clock.month)) {
          manual_clock.day = 1;
        }
        break;
      case 3:
        manual_clock.hour = (manual_clock.hour + delta + 24) % 24;
        break;
      case 4:
        manual_clock.minute = (manual_clock.minute + delta + 60) % 60;
        break;
      case 5:
        manual_clock.second = (manual_clock.second + delta + 60) % 60;
        break;
    }
    ClampManualClockState(&manual_clock);
  };

  auto ApplyManualClockToSystem = [&](std::string* error) -> bool {
    ClampManualClockState(&manual_clock);
    std::tm tm_set{};
    tm_set.tm_year = manual_clock.year - 1900;
    tm_set.tm_mon = manual_clock.month - 1;
    tm_set.tm_mday = manual_clock.day;
    tm_set.tm_hour = manual_clock.hour;
    tm_set.tm_min = manual_clock.minute;
    tm_set.tm_sec = manual_clock.second;
    tm_set.tm_isdst = -1;
    const std::time_t local_epoch = std::mktime(&tm_set);
    if (local_epoch < 0) {
      if (error) {
        *error = "mktime failed";
      }
      return false;
    }
#ifdef __linux__
    const timespec ts{local_epoch, 0};
    if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
      if (error) {
        *error = std::strerror(errno);
      }
      return false;
    }
    manual_time_set = true;
    LoadManualClockFromSystem();
    return true;
#else
    if (error) {
      *error = "clock_settime not supported on this platform";
    }
    return false;
#endif
  };

  LoadManualClockFromSystem();

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
  auto PrepareHeadphonePlaybackOutput = [&]() -> bool {
    snd_mixer_t* mixer = nullptr;
    snd_mixer_elem_t* elem = nullptr;
    snd_mixer_selem_id_t* sid = nullptr;
    bool ok = false;

    if (snd_mixer_open(&mixer, 0) < 0) {
      return false;
    }
    if (snd_mixer_attach(mixer, "hw:CARD=Headphones") < 0) {
      goto done;
    }
    if (snd_mixer_selem_register(mixer, nullptr, nullptr) < 0) {
      goto done;
    }
    if (snd_mixer_load(mixer) < 0) {
      goto done;
    }
    if (snd_mixer_selem_id_malloc(&sid) < 0 || !sid) {
      goto done;
    }
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "PCM");
    elem = snd_mixer_find_selem(mixer, sid);
    if (!elem) {
      goto done;
    }
    if (snd_mixer_selem_has_playback_switch(elem)) {
      if (snd_mixer_selem_set_playback_switch_all(elem, 1) < 0) {
        goto done;
      }
    }
    if (snd_mixer_selem_has_playback_volume(elem)) {
      long min_vol = 0;
      long max_vol = 0;
      snd_mixer_selem_get_playback_volume_range(elem, &min_vol, &max_vol);
      if (snd_mixer_selem_set_playback_volume_all(elem, max_vol) < 0) {
        goto done;
      }
    }
    ok = true;

  done:
    if (sid) {
      snd_mixer_selem_id_free(sid);
    }
    if (mixer) {
      snd_mixer_close(mixer);
    }
    return ok;
  };
  auto SetPlaybackInfo = [&](const std::string& info, bool is_error) {
    std::lock_guard<std::mutex> lock(playback_mutex);
    playback_info = info;
    playback_info_error = is_error;
  };
  auto RefreshPlaybackSelectionInfo = [&]() {
    std::string selected_path;
    {
      std::lock_guard<std::mutex> lock(playback_mutex);
      if (playback_files.empty() || playback_selected < 0 ||
          playback_selected >= static_cast<int>(playback_files.size())) {
        playback_info = "NO FILES";
        playback_info_error = true;
        playback_selected_duration_sec.store(0);
        playback_elapsed_sec.store(0);
        return;
      }
      selected_path = playback_files[playback_selected];
    }

    SF_INFO info{};
    SNDFILE* in = sf_open(selected_path.c_str(), SFM_READ, &info);
    if (!in) {
      playback_selected_duration_sec.store(0);
      SetPlaybackInfo("OPEN FAILED", true);
      return;
    }
    playback_selected_duration_sec.store(
        (info.samplerate > 0 && info.frames > 0)
            ? static_cast<uint64_t>(info.frames / info.samplerate)
            : 0);
    int left_idx = -1;
    int right_idx = -1;
    std::string route_label;
    const bool supported =
        DeterminePlaybackMapping(selected_path, info.channels, &left_idx,
                                &right_idx, &route_label);
    sf_close(in);
    SetPlaybackInfo(route_label.empty() ? "UNSUPPORTED" : route_label,
                    !supported);
  };
  auto RefreshPlaybackFiles = [&]() {
    auto files = ListPlaybackFiles();
    {
      std::lock_guard<std::mutex> lock(playback_mutex);
      playback_files = std::move(files);
      if (playback_files.empty()) {
        playback_selected = 0;
        playback_info = "NO FILES";
        playback_info_error = true;
        playback_selected_duration_sec.store(0);
        playback_elapsed_sec.store(0);
      } else {
        playback_selected = std::clamp(
            playback_selected, 0,
            static_cast<int>(playback_files.size()) - 1);
      }
    }
    if (!playback_files.empty()) {
      RefreshPlaybackSelectionInfo();
    }
  };
  auto StopPlayback = [&]() {
    playback_stop_requested.store(true);
    if (playback_thread.joinable()) {
      playback_thread.join();
    }
    playback_active.store(false);
    playback_stop_requested.store(false);
    playback_seek_seconds_pending.store(0);
    playback_elapsed_sec.store(0);
  };
  auto StartPlayback = [&]() -> bool {
    std::string selected_path;
    {
      std::lock_guard<std::mutex> lock(playback_mutex);
      if (playback_files.empty() || playback_selected < 0 ||
          playback_selected >= static_cast<int>(playback_files.size())) {
        playback_info = "NO FILES";
        playback_info_error = true;
        return false;
      }
      selected_path = playback_files[playback_selected];
    }

    SF_INFO info{};
    SNDFILE* probe = sf_open(selected_path.c_str(), SFM_READ, &info);
    if (!probe) {
      SetPlaybackInfo("OPEN FAILED", true);
      return false;
    }
    playback_selected_duration_sec.store(
        (info.samplerate > 0 && info.frames > 0)
            ? static_cast<uint64_t>(info.frames / info.samplerate)
            : 0);
    int left_idx = -1;
    int right_idx = -1;
    std::string route_label;
    const bool supported =
        DeterminePlaybackMapping(selected_path, info.channels, &left_idx,
                                &right_idx, &route_label);
    sf_close(probe);
    if (!supported) {
      SetPlaybackInfo(route_label.empty() ? "UNSUPPORTED" : route_label,
                      true);
      return false;
    }

    StopPlayback();
    playback_stop_requested.store(false);
    playback_seek_seconds_pending.store(0);
    playback_elapsed_sec.store(0);
    SetPlaybackInfo(route_label, false);
    playback_active.store(true);
    playback_thread = std::thread([&, selected_path, left_idx, right_idx,
                                   route_label]() {
      SF_INFO sfinfo{};
      SNDFILE* in = sf_open(selected_path.c_str(), SFM_READ, &sfinfo);
      if (!in) {
        SetPlaybackInfo("OPEN FAILED", true);
        playback_active.store(false);
        playback_elapsed_sec.store(0);
        return;
      }

      snd_pcm_t* out_pcm = nullptr;
      if (!PrepareHeadphonePlaybackOutput()) {
        std::fprintf(stderr, "Warning: failed to set Headphones PCM output to 100%%/unmuted\n");
      }
      int err = snd_pcm_open(&out_pcm, "plughw:CARD=Headphones,DEV=0",
                             SND_PCM_STREAM_PLAYBACK, 0);
      if (err < 0) {
        std::fprintf(stderr, "snd_pcm_open playback failed: %s\n",
                     snd_strerror(err));
        SetPlaybackInfo("PLAYBACK OPEN FAIL", true);
        sf_close(in);
        playback_active.store(false);
        playback_elapsed_sec.store(0);
        return;
      }

      snd_pcm_hw_params_t* params = nullptr;
      snd_pcm_hw_params_malloc(&params);
      snd_pcm_hw_params_any(out_pcm, params);
      unsigned int play_rate = static_cast<unsigned int>(sfinfo.samplerate);
      unsigned int period_time = 50000;
      unsigned int buffer_time = 200000;
      err = snd_pcm_hw_params_set_access(out_pcm, params,
                                         SND_PCM_ACCESS_RW_INTERLEAVED);
      if (err >= 0) err = snd_pcm_hw_params_set_format(out_pcm, params,
                                                       SND_PCM_FORMAT_S16_LE);
      if (err >= 0) err = snd_pcm_hw_params_set_channels(out_pcm, params, 2);
      if (err >= 0) err = snd_pcm_hw_params_set_rate_near(out_pcm, params,
                                                          &play_rate, nullptr);
      if (err >= 0) err = snd_pcm_hw_params_set_period_time_near(
          out_pcm, params, &period_time, nullptr);
      if (err >= 0) err = snd_pcm_hw_params_set_buffer_time_near(
          out_pcm, params, &buffer_time, nullptr);
      if (err >= 0) err = snd_pcm_hw_params(out_pcm, params);
      snd_pcm_hw_params_free(params);
      if (err < 0) {
        std::fprintf(stderr, "snd_pcm_hw_params playback failed: %s\n",
                     snd_strerror(err));
        SetPlaybackInfo("PLAYBACK HW FAIL", true);
        snd_pcm_close(out_pcm);
        sf_close(in);
        playback_active.store(false);
        playback_elapsed_sec.store(0);
        return;
      }

      snd_pcm_prepare(out_pcm);
      constexpr sf_count_t kChunkFrames = 2048;
      std::vector<float> in_frames(static_cast<size_t>(kChunkFrames) *
                                   static_cast<size_t>(sfinfo.channels));
      std::vector<int16_t> out_frames(static_cast<size_t>(kChunkFrames) * 2u);
      uint64_t frames_written_total = 0;
      bool playback_failed = false;

      while (!playback_stop_requested.load()) {
        const int64_t seek_seconds = playback_seek_seconds_pending.exchange(0);
        if (seek_seconds != 0) {
          const sf_count_t current_frame = sf_seek(in, 0, SF_SEEK_CUR);
          if (current_frame >= 0) {
            const int64_t delta_frames =
                seek_seconds * static_cast<int64_t>(sfinfo.samplerate);
            const int64_t max_frame = static_cast<int64_t>(sfinfo.frames);
            const int64_t target_frame = std::clamp(
                static_cast<int64_t>(current_frame) + delta_frames,
                static_cast<int64_t>(0), max_frame);
            if (sf_seek(in, static_cast<sf_count_t>(target_frame), SF_SEEK_SET) >=
                0) {
              snd_pcm_drop(out_pcm);
              snd_pcm_prepare(out_pcm);
              frames_written_total = static_cast<uint64_t>(target_frame);
              playback_elapsed_sec.store(
                  frames_written_total / static_cast<uint64_t>(sfinfo.samplerate));
            }
          }
        }

        const sf_count_t frames_read =
            sf_readf_float(in, in_frames.data(), kChunkFrames);
        if (frames_read <= 0) {
          break;
        }

        const float gain =
            std::pow(10.0f, static_cast<float>(playback_gain_db.load()) / 20.0f);
        for (sf_count_t frame = 0; frame < frames_read; ++frame) {
          const size_t base = static_cast<size_t>(frame) *
                              static_cast<size_t>(sfinfo.channels);
          const float left =
              in_frames[base + static_cast<size_t>(left_idx)] * gain;
          const float right =
              in_frames[base + static_cast<size_t>(right_idx)] * gain;
          const float l_clamped = std::max(-1.0f, std::min(1.0f, left));
          const float r_clamped = std::max(-1.0f, std::min(1.0f, right));
          const int l_int = static_cast<int>(
              l_clamped * 32767.0f + (l_clamped >= 0.0f ? 0.5f : -0.5f));
          const int r_int = static_cast<int>(
              r_clamped * 32767.0f + (r_clamped >= 0.0f ? 0.5f : -0.5f));
          out_frames[static_cast<size_t>(frame) * 2] =
              static_cast<int16_t>(std::max(-32768, std::min(32767, l_int)));
          out_frames[static_cast<size_t>(frame) * 2 + 1] =
              static_cast<int16_t>(std::max(-32768, std::min(32767, r_int)));
        }

        snd_pcm_sframes_t frames_left = frames_read;
        const int16_t* out_ptr = out_frames.data();
        while (frames_left > 0 && !playback_stop_requested.load()) {
          snd_pcm_sframes_t wrote = snd_pcm_writei(out_pcm, out_ptr, frames_left);
          if (wrote == -EPIPE) {
            snd_pcm_prepare(out_pcm);
            continue;
          }
          if (wrote < 0) {
            wrote = snd_pcm_recover(out_pcm, static_cast<int>(wrote), 1);
            if (wrote < 0) {
              std::fprintf(stderr, "snd_pcm_writei playback failed: %s\n",
                           snd_strerror(static_cast<int>(wrote)));
              SetPlaybackInfo("PLAYBACK WRITE FAIL", true);
              playback_failed = true;
              playback_stop_requested.store(true);
              break;
            }
            continue;
          }
          frames_left -= wrote;
          out_ptr += wrote * 2;
          frames_written_total += static_cast<uint64_t>(wrote);
          playback_elapsed_sec.store(
              frames_written_total / static_cast<uint64_t>(sfinfo.samplerate));
        }
      }

      const bool stopped_by_user = playback_stop_requested.load();
      if (stopped_by_user) {
        snd_pcm_drop(out_pcm);
      } else {
        snd_pcm_drain(out_pcm);
      }
      snd_pcm_close(out_pcm);
      sf_close(in);
      playback_active.store(false);
      playback_stop_requested.store(false);
      playback_elapsed_sec.store(0);
      if (!playback_failed && !stopped_by_user) {
        SetPlaybackInfo(route_label, false);
      }
    });
    return true;
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
    return BuildAutoOutPath(selected_mic, manual_time_set, next_take_number);
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
  auto ApplyCurrentMicConfig = [&]() -> bool {
    const bool setup_ok = SetupCapture();
    mic_available.store(setup_ok);
    if (!RefreshDerivedSizes()) {
      std::fprintf(stderr, "Failed to update capture sizes in idle mode\n");
      g_running.store(false);
      return false;
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
                   MicKindToString(selected_mic), opt.rate, current_channels,
                   AccessLabel());
    } else {
      std::fprintf(stdout,
                   "Selected: %s | %d Hz (mic not connected, recording disabled)\n",
                   MicKindToString(selected_mic), opt.rate);
    }
    return true;
  };
  const std::string out_preview =
      opt.out_overridden
          ? opt.out_path
          : BuildAutoOutPath(selected_mic, manual_time_set, next_take_number);
  RefreshZyliaGain();
  RefreshPlaybackFiles();
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
                  "LEFT/RIGHT cycle spcmic, zylia, playback, settings (seek while playing) | KEY2 MON/REC or play/save | KEY1 stop, playback stop, or settings edit/cancel | Ctrl+C exit.\n",
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
      constexpr size_t kWriterBatchPeriods = 4;
      std::vector<uint8_t> out;
      out.resize(period_bytes * kWriterBatchPeriods);
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
                             : BuildAutoOutPath(selected_mic, manual_time_set,
                                                next_take_number);
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
    ++next_take_number;
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
    RefreshPlaybackFiles();
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
    snap.playback_mode = (current_ui_mode == UiMode::kPlayback);
    snap.settings_mode = (current_ui_mode == UiMode::kSettings);
    snap.playback_active = playback_active.load();
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
    snap.playback_gain_db = playback_gain_db.load();
    snap.playback_elapsed_sec = snap.playback_active
                                    ? playback_elapsed_sec.load()
                                    : playback_selected_duration_sec.load();
    snap.settings_date = FormatManualClockDate(manual_clock);
    snap.settings_time = FormatManualClockTime(manual_clock);
    snap.settings_editing = settings_editing;
    snap.settings_field_index = manual_clock_field;
    {
      std::lock_guard<std::mutex> lock(ring_mutex);
      if (ring.capacity() > 0) {
        const size_t pct = (ring.size() * 100) / ring.capacity();
        snap.ring_fill_pct = static_cast<int>(std::min<size_t>(pct, 100));
      }
    }
    {
      std::lock_guard<std::mutex> lock(playback_mutex);
      snap.playback_info = playback_info;
      snap.playback_info_error = playback_info_error;
      if (snap.playback_mode) {
        const int total = static_cast<int>(playback_files.size());
        if (total > 0) {
          const int selected = std::clamp(playback_selected, 0, total - 1);
          int start = std::max(0, selected - static_cast<int>(kPlaybackVisibleItems / 2));
          if (start + static_cast<int>(kPlaybackVisibleItems) > total) {
            start = std::max(0, total - static_cast<int>(kPlaybackVisibleItems));
          }
          const int end = std::min(total, start + static_cast<int>(kPlaybackVisibleItems));
          for (int i = start; i < end; ++i) {
            snap.playback_items.push_back(MakePlaybackDisplayName(playback_files[i]));
          }
          snap.playback_selected_index = selected - start;
        }
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
                            joy_ud_repeat.load(), joy_lr_repeat.load());
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
    const bool is_play = playback_active.load();
    joy_ud_repeat.store((current_ui_mode == UiMode::kSettings && settings_editing) ||
                        (current_ui_mode == UiMode::kPlayback && is_play) ||
                        (selected_mic == MicKind::kZylia &&
                         (current_ui_mode == UiMode::kZylia || is_mon || is_rec)));
    joy_lr_repeat.store(current_ui_mode == UiMode::kPlayback && is_play);

    if (opt.hat_ui && !is_rec && !is_mon) {
      const bool left_evt = mic_left_requested.exchange(false);
      const bool right_evt = mic_right_requested.exchange(false);
      const bool up_evt = rate_up_requested.exchange(false);
      const bool down_evt = rate_down_requested.exchange(false);

      if (current_ui_mode == UiMode::kSettings) {
        if (!settings_editing) {
          start_requested.store(false);
          if (stop_requested.exchange(false)) {
            LoadManualClockFromSystem();
            manual_clock_field = 0;
            settings_editing = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
          }
          if (left_evt || right_evt) {
            if (selected_mic == MicKind::kSpcmic) {
              spcmic_rate_hz = opt.rate;
            }
            const UiMode next_mode = left_evt ? PrevUiMode(current_ui_mode)
                                              : NextUiMode(current_ui_mode);
            current_ui_mode = next_mode;
            if (current_ui_mode == UiMode::kPlayback) {
              RefreshPlaybackFiles();
              stop_requested.store(false);
              std::this_thread::sleep_for(std::chrono::milliseconds(20));
              continue;
            }
            selected_mic =
                (current_ui_mode == UiMode::kSpcmic) ? MicKind::kSpcmic : MicKind::kZylia;
            ApplyMicPreset(selected_mic);
            opt.rate = (selected_mic == MicKind::kSpcmic) ? spcmic_rate_hz : 48000;
            if (!ApplyCurrentMicConfig()) {
              break;
            }
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          continue;
        }
        if (stop_requested.exchange(false)) {
          LoadManualClockFromSystem();
          manual_clock_field = 0;
          settings_editing = false;
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          continue;
        }
        if (left_evt) {
          manual_clock_field = (manual_clock_field + 5) % 6;
        }
        if (right_evt) {
          manual_clock_field = (manual_clock_field + 1) % 6;
        }
        if (up_evt) {
          AdjustManualClockField(manual_clock_field, 1);
        }
        if (down_evt) {
          AdjustManualClockField(manual_clock_field, -1);
        }
        if (start_requested.exchange(false)) {
          std::string clock_error;
          if (ApplyManualClockToSystem(&clock_error)) {
            RefreshStorageRemaining();
            settings_editing = false;
            manual_clock_field = 0;
            std::fprintf(stdout, "System time set to %s %s\n",
                         FormatManualClockDate(manual_clock).c_str(),
                         FormatManualClockTime(manual_clock).c_str());
          } else {
            std::fprintf(stderr, "Failed to set system time: %s\n",
                         clock_error.c_str());
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }

      if (current_ui_mode == UiMode::kPlayback) {
        if (is_play) {
          if (stop_requested.exchange(false)) {
            StopPlayback();
            RefreshPlaybackSelectionInfo();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
          }
          start_requested.store(false);
          constexpr int64_t kPlaybackSeekStepSec = 5;
          if (left_evt) {
            playback_seek_seconds_pending.fetch_sub(kPlaybackSeekStepSec);
          }
          if (right_evt) {
            playback_seek_seconds_pending.fetch_add(kPlaybackSeekStepSec);
          }
          int gain_db = playback_gain_db.load();
          if (up_evt) {
            gain_db = std::min(80, gain_db + 1);
          }
          if (down_evt) {
            gain_db = std::max(0, gain_db - 1);
          }
          playback_gain_db.store(gain_db);
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          continue;
        }

        if (left_evt || right_evt) {
          const UiMode next_mode = left_evt ? PrevUiMode(current_ui_mode)
                                            : NextUiMode(current_ui_mode);
          current_ui_mode = next_mode;
          if (next_mode == UiMode::kSpcmic || next_mode == UiMode::kZylia) {
            selected_mic =
                (next_mode == UiMode::kSpcmic) ? MicKind::kSpcmic : MicKind::kZylia;
            ApplyMicPreset(selected_mic);
            opt.rate = (selected_mic == MicKind::kSpcmic) ? spcmic_rate_hz : 48000;
            if (!ApplyCurrentMicConfig()) {
              break;
            }
          } else if (next_mode == UiMode::kPlayback) {
            RefreshPlaybackFiles();
          } else if (next_mode == UiMode::kSettings) {
            LoadManualClockFromSystem();
            manual_clock_field = 0;
            settings_editing = false;
          }
        }

        if (up_evt || down_evt) {
          int delta = 0;
          if (up_evt) {
            delta -= 1;
          }
          if (down_evt) {
            delta += 1;
          }
          if (delta != 0) {
            {
              std::lock_guard<std::mutex> lock(playback_mutex);
              if (!playback_files.empty()) {
                playback_selected = std::clamp(
                    playback_selected + delta, 0,
                    static_cast<int>(playback_files.size()) - 1);
              }
            }
            RefreshPlaybackSelectionInfo();
          }
        }

        if (start_requested.exchange(false)) {
          if (!StartPlayback()) {
            std::fprintf(stderr, "Failed to start playback\n");
          }
        }
        stop_requested.store(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }

      if (left_evt || right_evt) {
        const UiMode next_mode = left_evt ? PrevUiMode(current_ui_mode)
                                          : NextUiMode(current_ui_mode);
        if (next_mode != current_ui_mode) {
          if (selected_mic == MicKind::kSpcmic) {
            spcmic_rate_hz = opt.rate;
          }
          current_ui_mode = next_mode;
          if (current_ui_mode == UiMode::kPlayback) {
            RefreshPlaybackFiles();
            stop_requested.store(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
          }
          if (current_ui_mode == UiMode::kSettings) {
            LoadManualClockFromSystem();
            manual_clock_field = 0;
            settings_editing = false;
            stop_requested.store(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
          }
          selected_mic =
              (current_ui_mode == UiMode::kSpcmic) ? MicKind::kSpcmic : MicKind::kZylia;
          ApplyMicPreset(selected_mic);
          opt.rate = (selected_mic == MicKind::kSpcmic) ? spcmic_rate_hz : 48000;
          if (!ApplyCurrentMicConfig()) {
            break;
          }
        }
      }

      bool rate_changed = false;
      int gain_delta = 0;
      if (current_ui_mode == UiMode::kSpcmic) {
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
      } else if (current_ui_mode == UiMode::kZylia) {
        if (up_evt) {
          gain_delta += 1;
        }
        if (down_evt) {
          gain_delta -= 1;
        }
      }

      if (rate_changed) {
        if (!ApplyCurrentMicConfig()) {
          break;
        }
      }
      if (current_ui_mode == UiMode::kZylia && gain_delta != 0 &&
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
  if (playback_active.load() || playback_thread.joinable()) {
    StopPlayback();
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


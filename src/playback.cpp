#include "playback.h"

#include "path_utils.h"

std::vector<std::string> ListPlaybackFiles(const std::string& root_dir) {
  std::vector<std::string> files;
  std::error_code ec;
  const std::filesystem::path root(root_dir);
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
    if (ext == ".wav" || ext == ".rf64") {
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

std::string TruncateDisplayName(std::string text, size_t max_len) {
  if (text.size() > max_len) {
    text.resize(max_len);
  }
  return text;
}

std::string MakePlaybackDisplayName(const std::string& file_path) {
  return TruncateDisplayName(std::filesystem::path(file_path).stem().string(),
                             18);
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

bool PrepareHeadphonePlaybackOutput() {
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
}

void SetPlaybackInfo(PlaybackSession& playback, const std::string& info,
                     bool is_error) {
  std::lock_guard<std::mutex> lock(playback.mutex);
  playback.info = info;
  playback.info_error = is_error;
}

void RefreshPlaybackSelectionInfo(PlaybackSession& playback) {
  std::string selected_path;
  {
    std::lock_guard<std::mutex> lock(playback.mutex);
    if (playback.files.empty() || playback.selected < 0 ||
        playback.selected >= static_cast<int>(playback.files.size())) {
      playback.info = "NO FILES";
      playback.info_error = true;
      playback.selected_duration_sec.store(0);
      playback.elapsed_sec.store(0);
      return;
    }
    selected_path = playback.files[playback.selected];
  }

  SF_INFO info{};
  SNDFILE* in = sf_open(selected_path.c_str(), SFM_READ, &info);
  if (!in) {
    playback.selected_duration_sec.store(0);
    SetPlaybackInfo(playback, "OPEN FAILED", true);
    return;
  }
  playback.selected_duration_sec.store(
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
  SetPlaybackInfo(playback, route_label.empty() ? "UNSUPPORTED" : route_label,
                  !supported);
}

void RefreshPlaybackFiles(PlaybackSession& playback, const std::string& root_dir) {
  auto files = ListPlaybackFiles(root_dir);
  bool has_files = false;
  {
    std::lock_guard<std::mutex> lock(playback.mutex);
    playback.files = std::move(files);
    if (playback.files.empty()) {
      playback.selected = 0;
      playback.info = "NO FILES";
      playback.info_error = true;
      playback.selected_duration_sec.store(0);
      playback.elapsed_sec.store(0);
    } else {
      playback.selected = std::clamp(
          playback.selected, 0, static_cast<int>(playback.files.size()) - 1);
      has_files = true;
    }
  }
  if (has_files) {
    RefreshPlaybackSelectionInfo(playback);
  }
}

void StopPlayback(PlaybackSession& playback) {
  playback.stop_requested.store(true);
  if (playback.thread.joinable()) {
    playback.thread.join();
  }
  playback.active.store(false);
  playback.stop_requested.store(false);
  playback.seek_seconds_pending.store(0);
  playback.elapsed_sec.store(0);
}

bool StartPlayback(PlaybackSession& playback) {
  std::string selected_path;
  {
    std::lock_guard<std::mutex> lock(playback.mutex);
    if (playback.files.empty() || playback.selected < 0 ||
        playback.selected >= static_cast<int>(playback.files.size())) {
      playback.info = "NO FILES";
      playback.info_error = true;
      return false;
    }
    selected_path = playback.files[playback.selected];
  }

  SF_INFO info{};
  SNDFILE* probe = sf_open(selected_path.c_str(), SFM_READ, &info);
  if (!probe) {
    SetPlaybackInfo(playback, "OPEN FAILED", true);
    return false;
  }
  playback.selected_duration_sec.store(
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
    SetPlaybackInfo(playback, route_label.empty() ? "UNSUPPORTED" : route_label,
                    true);
    return false;
  }

  StopPlayback(playback);
  playback.stop_requested.store(false);
  playback.seek_seconds_pending.store(0);
  playback.elapsed_sec.store(0);
  SetPlaybackInfo(playback, route_label, false);
  playback.active.store(true);
  playback.thread = std::thread([&, selected_path, left_idx, right_idx, route_label]() {
    SF_INFO sfinfo{};
    SNDFILE* in = sf_open(selected_path.c_str(), SFM_READ, &sfinfo);
    if (!in) {
      SetPlaybackInfo(playback, "OPEN FAILED", true);
      playback.active.store(false);
      playback.elapsed_sec.store(0);
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
      SetPlaybackInfo(playback, "PLAYBACK OPEN FAIL", true);
      sf_close(in);
      playback.active.store(false);
      playback.elapsed_sec.store(0);
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
      SetPlaybackInfo(playback, "PLAYBACK HW FAIL", true);
      snd_pcm_close(out_pcm);
      sf_close(in);
      playback.active.store(false);
      playback.elapsed_sec.store(0);
      return;
    }

    snd_pcm_prepare(out_pcm);
    constexpr sf_count_t kChunkFrames = 2048;
    std::vector<float> in_frames(static_cast<size_t>(kChunkFrames) *
                                 static_cast<size_t>(sfinfo.channels));
    std::vector<int16_t> out_frames(static_cast<size_t>(kChunkFrames) * 2u);
    uint64_t frames_written_total = 0;
    bool playback_failed = false;

    while (!playback.stop_requested.load()) {
      const int64_t seek_seconds = playback.seek_seconds_pending.exchange(0);
      if (seek_seconds != 0) {
        const sf_count_t current_frame = sf_seek(in, 0, SF_SEEK_CUR);
        if (current_frame >= 0) {
          const int64_t delta_frames =
              seek_seconds * static_cast<int64_t>(sfinfo.samplerate);
          const int64_t max_frame = static_cast<int64_t>(sfinfo.frames);
          const int64_t target_frame = std::clamp(
              static_cast<int64_t>(current_frame) + delta_frames,
              static_cast<int64_t>(0), max_frame);
          if (sf_seek(in, static_cast<sf_count_t>(target_frame), SF_SEEK_SET) >= 0) {
            snd_pcm_drop(out_pcm);
            snd_pcm_prepare(out_pcm);
            frames_written_total = static_cast<uint64_t>(target_frame);
            playback.elapsed_sec.store(
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
          std::pow(10.0f, static_cast<float>(playback.gain_db.load()) / 20.0f);
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
      while (frames_left > 0 && !playback.stop_requested.load()) {
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
            SetPlaybackInfo(playback, "PLAYBACK WRITE FAIL", true);
            playback_failed = true;
            playback.stop_requested.store(true);
            break;
          }
          continue;
        }
        frames_left -= wrote;
        out_ptr += wrote * 2;
        frames_written_total += static_cast<uint64_t>(wrote);
        playback.elapsed_sec.store(
            frames_written_total / static_cast<uint64_t>(sfinfo.samplerate));
      }
    }

    const bool stopped_by_user = playback.stop_requested.load();
    if (stopped_by_user) {
      snd_pcm_drop(out_pcm);
    } else {
      snd_pcm_drain(out_pcm);
    }
    snd_pcm_close(out_pcm);
    sf_close(in);
    playback.active.store(false);
    playback.stop_requested.store(false);
    playback.elapsed_sec.store(0);
    if (!playback_failed && !stopped_by_user) {
      SetPlaybackInfo(playback, route_label, false);
    }
  });
  return true;
}

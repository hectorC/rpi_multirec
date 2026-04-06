#pragma once

#include "common.h"

struct PlaybackSession {
  std::mutex mutex;
  std::vector<std::string> files;
  std::string info = "NO FILES";
  bool info_error = true;
  int selected = 0;
  std::thread thread;
  std::atomic<bool> active{false};
  std::atomic<bool> stop_requested{false};
  std::atomic<int> gain_db{0};
  std::atomic<uint64_t> elapsed_sec{0};
  std::atomic<uint64_t> selected_duration_sec{0};
  std::atomic<int64_t> seek_seconds_pending{0};
};

constexpr int kSpcmicPlaybackLeftChannel = 24;
constexpr int kSpcmicPlaybackRightChannel = 52;
constexpr int kZyliaPlaybackLeftChannel = 4;
constexpr int kZyliaPlaybackRightChannel = 7;
constexpr size_t kPlaybackVisibleItems = 6;

std::vector<std::string> ListPlaybackFiles(const std::string& root_dir);
std::string TruncateDisplayName(std::string text, size_t max_len);
std::string MakePlaybackDisplayName(const std::string& file_path);
bool DeterminePlaybackMapping(const std::string& file_path, int file_channels,
                              int* left_idx, int* right_idx,
                              std::string* route_label);
bool PrepareHeadphonePlaybackOutput();
void SetPlaybackInfo(PlaybackSession& playback, const std::string& info,
                     bool is_error);
void RefreshPlaybackSelectionInfo(PlaybackSession& playback);
void RefreshPlaybackFiles(PlaybackSession& playback, const std::string& root_dir);
void StopPlayback(PlaybackSession& playback);
bool StartPlayback(PlaybackSession& playback);

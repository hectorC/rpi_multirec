#pragma once

#include "common.h"

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
  int ring_ms = 20000;
  int status_ms = 0;
  bool list_devices = false;
  bool show_help = false;
};

inline const char* MicKindToString(MicKind mic) {
  if (mic == MicKind::kSpcmic) {
    return "spcmic";
  }
  if (mic == MicKind::kZylia) {
    return "zylia";
  }
  return "";
}


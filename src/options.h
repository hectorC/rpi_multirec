#pragma once

#include "common.h"
#include "types.h"
#include "alsa_utils.h"
#include "path_utils.h"

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
      "  --buffer-ms <ms>        ALSA buffer time in ms (default: 500)\n"
      "  --period-ms <ms>        ALSA period time in ms (default: 50)\n"
      "  --ring-ms <ms>          Ring buffer time in ms (default: 20000)\n"
      "  --status-ms <ms>        Print status every N ms (default: 0=off)\n"
      "  -L, --list-devices      List ALSA PCM devices and exit\n"
      "  -h, --help              Show this help\n"
      "\n"
      "Mic presets:\n"
      "  spcmic => device=auto-detect spacemic card channels=84 access=rw\n"
      "  zylia  => device=auto-detect Zylia card    channels=19 access=mmap\n"
      "  Explicit --device/--channels/--access override preset values.\n"
      "\n"
      "Auto naming:\n"
      "  If --out is omitted and time was not set in the HAT settings page:\n"
      "  <mic>_T0001.rf64\n"
      "  If time was manually set in the HAT settings page:\n"
      "  <mic>_YYYYMMDD_HHMMSS.rf64\n"
      "  Default output directory: /srv/rpi_multirec/recordings\n"
      "  If exFAT external storage is mounted at startup, files go to\n"
      "  <mount>/rpi_multirec instead.\n"
      "  Auto naming requires --mic spcmic|zylia.\n"
      "\n"
      "Waveshare HAT controls:\n"
      "  With --hat-ui the app starts in IDLE.\n"
      "  Joystick LEFT/RIGHT cycles spcmic -> zylia -> playback -> settings\n"
      "  KEY2 = MON from IDLE, then KEY2 again = REC\n"
      "  KEY1 = stop (MON->IDLE, REC->stop/finalize, playback->stop)\n"
      "  KEY3 short release = backlight toggle\n"
      "  KEY3 hold 5s = power off after clean shutdown\n"
      "  SPCMIC: Joystick UP/DOWN (IDLE only) = select 96kHz/48kHz\n"
      "  ZYLIA: Joystick UP/DOWN (IDLE/MON/REC) = Master Gain +/-1 dB (hold to repeat)\n"
      "  PLAYBACK: Joystick UP/DOWN browses files, then adjusts volume while playing\n"
      "  PLAYBACK: hold Joystick LEFT/RIGHT while playing to seek backward/forward\n"
      "  SETTINGS: KEY1 enters edit mode; outside edit, LEFT/RIGHT changes page\n"
      "  SETTINGS EDIT: LEFT/RIGHT selects field, UP/DOWN changes value,\n"
      "                 KEY1 cancels, KEY2 sets time\n"
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
      out->device = FindPreferredCaptureDevice({"spacemic"},
                                               "hw:CARD=s02E5D5,DEV=0")
                        .hw_device;
    }
    if (!out->channels_overridden) {
      out->channels = 84;
    }
    if (!out->access_overridden) {
      out->access = SND_PCM_ACCESS_RW_INTERLEAVED;
    }
  } else if (out->mic == MicKind::kZylia) {
    if (!out->device_overridden) {
      out->device = FindPreferredCaptureDevice({"zylia", "zm-1", "zm1"},
                                               "hw:CARD=ZM13E,DEV=0",
                                               "hw:CARD=ZM13E")
                        .hw_device;
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

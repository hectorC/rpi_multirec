#pragma once

#include "common.h"

struct AlsaDeviceMatch {
  std::string hw_device;
  std::string mixer_card;
};

bool NameHasKeyword(const std::string& text,
                    std::initializer_list<const char*> keywords) {
  std::string lower = text;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const char* keyword : keywords) {
    if (lower.find(keyword) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string MixerCardFromPcmDevice(const std::string& device) {
  const std::string needle = "CARD=";
  const size_t card_pos = device.find(needle);
  if (card_pos == std::string::npos) {
    return {};
  }
  const size_t value_pos = card_pos + needle.size();
  size_t end_pos = device.find(',', value_pos);
  if (end_pos == std::string::npos) {
    end_pos = device.size();
  }
  if (end_pos <= value_pos) {
    return {};
  }
  return "hw:CARD=" + device.substr(value_pos, end_pos - value_pos);
}

AlsaDeviceMatch FindPreferredCaptureDevice(
    std::initializer_list<const char*> keywords,
    const std::string& fallback_hw_device,
    const std::string& fallback_mixer_card = {}) {
  AlsaDeviceMatch best{};
  best.hw_device = fallback_hw_device;
  best.mixer_card = fallback_mixer_card;

  void** hints = nullptr;
  const int rc = snd_device_name_hint(-1, "pcm", &hints);
  if (rc < 0) {
    return best;
  }

  std::string fallback_candidate;
  for (void** it = hints; it && *it; ++it) {
    char* name = snd_device_name_get_hint(*it, "NAME");
    char* desc = snd_device_name_get_hint(*it, "DESC");
    char* ioid = snd_device_name_get_hint(*it, "IOID");

    const std::string name_str = name ? name : "";
    const std::string desc_str = desc ? desc : "";
    const std::string ioid_str = ioid ? ioid : "";
    const bool is_input = ioid_str.empty() || ioid_str == "Input";
    const bool matches = is_input &&
                         (NameHasKeyword(name_str, keywords) ||
                          NameHasKeyword(desc_str, keywords));

    if (matches) {
      if (name_str.rfind("hw:", 0) == 0) {
        best.hw_device = name_str;
        if (best.mixer_card.empty()) {
          best.mixer_card = MixerCardFromPcmDevice(name_str);
        }
        std::free(name);
        if (desc) std::free(desc);
        if (ioid) std::free(ioid);
        snd_device_name_free_hint(hints);
        return best;
      }
      if (fallback_candidate.empty() && name_str.rfind("plughw:", 0) == 0) {
        fallback_candidate = name_str;
      }
    }

    if (name) std::free(name);
    if (desc) std::free(desc);
    if (ioid) std::free(ioid);
  }

  snd_device_name_free_hint(hints);
  if (!fallback_candidate.empty()) {
    best.hw_device = fallback_candidate;
    if (best.mixer_card.empty()) {
      best.mixer_card = MixerCardFromPcmDevice(fallback_candidate);
    }
  }
  return best;
}

bool ElevateCurrentThreadForCapture(int requested_priority, std::string* error,
                                    int* applied_priority) {
#ifdef __linux__
  const int max_priority = sched_get_priority_max(SCHED_FIFO);
  if (max_priority < 1) {
    if (error) {
      *error = "sched_get_priority_max(SCHED_FIFO) failed";
    }
    return false;
  }

  sched_param param {};
  param.sched_priority = std::max(1, std::min(requested_priority, max_priority));
  if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
    if (error) {
      *error = std::strerror(errno);
    }
    return false;
  }
  if (applied_priority) {
    *applied_priority = param.sched_priority;
  }
  return true;
#else
  (void)requested_priority;
  (void)error;
  (void)applied_priority;
  return false;
#endif
}

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

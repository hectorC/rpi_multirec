#pragma once

#include "common.h"

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

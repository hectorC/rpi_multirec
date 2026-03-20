#pragma once

#include "common.h"
#include "audio_utils.h"

#ifdef __linux__

struct UiSnapshot {
  bool recording = false;
  bool monitoring = false;
  bool external_storage = false;
  std::string mic;
  bool mic_connected = true;
  bool battery_valid = false;
  int battery_pct = 0;
  bool storage_valid = false;
  uint64_t remaining_storage_sec = 0;
  bool finalize_pending = false;
  unsigned int rate = 0;
  int channels = 0;
  bool zylia_gain_valid = false;
  int zylia_gain_db = 0;
  int peak_pct = 0;
  uint64_t xruns = 0;
  uint64_t dropped_bytes = 0;
  int ring_fill_pct = 0;
  uint64_t elapsed_sec = 0;
};

class WaveshareHatUi {
 public:
  const std::string& LastError() const { return last_error_; }

  bool Init() {
    last_error_.clear();

    chip_ = gpiod_chip_open("/dev/gpiochip0");
    if (!chip_) {
      return FailErrno("gpiod_chip_open(/dev/gpiochip0)");
    }

    if (!RequestLine(&dc_, kPinDc, true, 1, "DC")) {
      return false;
    }
    if (!RequestLine(&rst_, kPinRst, true, 1, "RST")) {
      return false;
    }
    if (!RequestLine(&bl_, kPinBl, true, 1, "BL")) {
      return false;
    }
    if (!RequestLine(&btn_[0], kPinKey1, false, 0, "KEY1")) {
      return false;
    }
    if (!RequestLine(&btn_[1], kPinKey2, false, 0, "KEY2")) {
      return false;
    }
    if (!RequestLine(&btn_[2], kPinKey3, false, 0, "KEY3")) {
      return false;
    }
    if (!RequestLine(&btn_[3], kPinUp, false, 0, "UP")) {
      return false;
    }
    if (!RequestLine(&btn_[4], kPinDown, false, 0, "DOWN")) {
      return false;
    }
    if (!RequestLine(&btn_[5], kPinLeft, false, 0, "LEFT")) {
      return false;
    }
    if (!RequestLine(&btn_[6], kPinRight, false, 0, "RIGHT")) {
      return false;
    }
    if (!RequestLine(&btn_[7], kPinPress, false, 0, "PRESS")) {
      return false;
    }

    for (int i = 0; i < 8; ++i) {
      last_btn_[i] = ReadGpio(btn_[i]);
      if (last_btn_[i] < 0) {
        last_btn_[i] = 1;
      }
      idle_btn_[i] = last_btn_[i];
    }

    spi_fd_ = open("/dev/spidev0.0", O_RDWR);
    if (spi_fd_ < 0) {
      return FailErrno("open(/dev/spidev0.0)");
    }
    uint8_t mode = 0;
    uint8_t bits = 8;
    uint32_t speed = 32000000;
    if (ioctl(spi_fd_, SPI_IOC_WR_MODE, &mode) < 0) {
      return FailErrno("ioctl(SPI_IOC_WR_MODE)");
    }
    if (ioctl(spi_fd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) {
      return FailErrno("ioctl(SPI_IOC_WR_BITS_PER_WORD)");
    }
    if (ioctl(spi_fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
      return FailErrno("ioctl(SPI_IOC_WR_MAX_SPEED_HZ)");
    }

    if (!WriteGpio(bl_, 1)) return FailErrno("WriteGpio(BL=1)");
    if (!WriteGpio(rst_, 1)) return FailErrno("WriteGpio(RST=1)");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!WriteGpio(rst_, 0)) return FailErrno("WriteGpio(RST=0)");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!WriteGpio(rst_, 1)) return FailErrno("WriteGpio(RST=1)");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    if (!WriteCmd(0x01, nullptr, 0)) return Fail("WriteCmd(SWRESET)");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    if (!WriteCmd(0x11, nullptr, 0)) return Fail("WriteCmd(SLPOUT)");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const uint8_t madctl[] = {0x00};
    const uint8_t colmod[] = {0x55};  // RGB565
    if (!WriteCmd(0x36, madctl, sizeof(madctl))) return Fail("WriteCmd(MADCTL)");
    if (!WriteCmd(0x3A, colmod, sizeof(colmod))) return Fail("WriteCmd(COLMOD)");
    if (!WriteCmd(0x21, nullptr, 0)) return Fail("WriteCmd(INVON)");
    if (!WriteCmd(0x29, nullptr, 0)) return Fail("WriteCmd(DISPON)");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    Clear(kBlack);
    if (!Flush()) return Fail("Flush(initial)");
    return true;
  }

  void Shutdown() {
    if (spi_fd_ >= 0) close(spi_fd_);
    spi_fd_ = -1;
    ReleaseLine(dc_);
    ReleaseLine(rst_);
    ReleaseLine(bl_);
    for (int i = 0; i < 8; ++i) {
      ReleaseLine(btn_[i]);
    }
    if (chip_) {
      gpiod_chip_close(chip_);
      chip_ = nullptr;
    }
  }

  void PollButtons(bool* start, bool* stop, bool* mic_left, bool* mic_right,
                   bool* rate_up, bool* rate_down, bool* backlight_toggle,
                   bool* poweroff, bool repeat_ud = false) {
    if (start) *start = false;
    if (stop) *stop = false;
    if (mic_left) *mic_left = false;
    if (mic_right) *mic_right = false;
    if (rate_up) *rate_up = false;
    if (rate_down) *rate_down = false;
    if (backlight_toggle) *backlight_toggle = false;
    if (poweroff) *poweroff = false;
    const int key1 = ReadGpio(btn_[0]);
    const int key2 = ReadGpio(btn_[1]);
    const int key3 = ReadGpio(btn_[2]);
    const int joy_up = ReadGpio(btn_[3]);
    const int joy_down = ReadGpio(btn_[4]);
    const int joy_left = ReadGpio(btn_[5]);
    const int joy_right = ReadGpio(btn_[6]);
    auto edge = [&](int idx, int value) -> bool {
      return value >= 0 && last_btn_[idx] == idle_btn_[idx] &&
             value != idle_btn_[idx];
    };
    const auto now = std::chrono::steady_clock::now();
    auto pressed = [&](int idx, int value) -> bool {
      return value >= 0 && value != idle_btn_[idx];
    };
    auto was_pressed = [&](int idx) -> bool {
      return last_btn_[idx] != idle_btn_[idx];
    };
    auto hold_repeat = [&](int idx, int value, bool* out) {
      if (!out) {
        return;
      }
      if (pressed(idx, value)) {
        if (!was_pressed(idx)) {
          *out = true;
          next_repeat_[idx] = now + std::chrono::milliseconds(350);
        } else if (repeat_ud && now >= next_repeat_[idx]) {
          *out = true;
          next_repeat_[idx] = now + std::chrono::milliseconds(120);
        }
      } else {
        next_repeat_[idx] = std::chrono::steady_clock::time_point{};
      }
    };

    if (stop && edge(0, key1)) {
      *stop = true;
    }
    if (start && edge(1, key2)) {
      *start = true;
    }
    if (pressed(2, key3) && !was_pressed(2)) {
      key3_press_started_ = now;
      key3_poweroff_fired_ = false;
    } else if (pressed(2, key3) && was_pressed(2) && !key3_poweroff_fired_ &&
               now - key3_press_started_ >= std::chrono::seconds(5)) {
      key3_poweroff_fired_ = true;
      if (poweroff) {
        *poweroff = true;
      }
    } else if (!pressed(2, key3) && was_pressed(2)) {
      if (!key3_poweroff_fired_ && backlight_toggle) {
        *backlight_toggle = true;
      }
      key3_press_started_ = std::chrono::steady_clock::time_point{};
      key3_poweroff_fired_ = false;
    }
    if (mic_left && edge(5, joy_left)) {
      *mic_left = true;
    }
    if (mic_right && edge(6, joy_right)) {
      *mic_right = true;
    }
    hold_repeat(3, joy_up, rate_up);
    hold_repeat(4, joy_down, rate_down);
    if (key1 >= 0) last_btn_[0] = key1;
    if (key2 >= 0) last_btn_[1] = key2;
    if (key3 >= 0) last_btn_[2] = key3;
    if (joy_up >= 0) last_btn_[3] = joy_up;
    if (joy_down >= 0) last_btn_[4] = joy_down;
    if (joy_left >= 0) last_btn_[5] = joy_left;
    if (joy_right >= 0) last_btn_[6] = joy_right;
  }

  void ToggleBacklight() {
    backlight_on_ = !backlight_on_;
    WriteGpio(bl_, backlight_on_ ? 1 : 0);
  }

  bool ShowPoweroffMessage() {
    backlight_on_ = true;
    WriteGpio(bl_, 1);
    Clear(kBlack);
    constexpr const char* kTitle = "Powering off!";
    constexpr const char* kLine1 = "Wait for the";
    constexpr const char* kLine2 = "green LED to";
    constexpr const char* kLine3 = "stop blinking,";
    constexpr const char* kLine4 = "then";
    constexpr const char* kLine5 = "turn switch off";
    constexpr int kTitleScale = 3;
    constexpr int kBodyScale = 2;

    const int title_w = static_cast<int>(std::strlen(kTitle)) * 6 * kTitleScale;
    const int title_x = std::max(0, (kWidth - title_w) / 2);
    DrawText(title_x, 18, kTitle, kWhite, kTitleScale);

    const int line1_w = static_cast<int>(std::strlen(kLine1)) * 6 * kBodyScale;
    const int line2_w = static_cast<int>(std::strlen(kLine2)) * 6 * kBodyScale;
    const int line3_w = static_cast<int>(std::strlen(kLine3)) * 6 * kBodyScale;
    const int line4_w = static_cast<int>(std::strlen(kLine4)) * 6 * kBodyScale;
    const int line5_w = static_cast<int>(std::strlen(kLine5)) * 6 * kBodyScale;
    DrawText(std::max(0, (kWidth - line1_w) / 2), 78, kLine1, kCyan, kBodyScale);
    DrawText(std::max(0, (kWidth - line2_w) / 2), 98, kLine2, kCyan, kBodyScale);
    DrawText(std::max(0, (kWidth - line3_w) / 2), 118, kLine3, kCyan, kBodyScale);
    DrawText(std::max(0, (kWidth - line4_w) / 2), 138, kLine4, kCyan, kBodyScale);
    DrawText(std::max(0, (kWidth - line5_w) / 2), 158, kLine5, kCyan, kBodyScale);
    return Flush();
  }

  bool Render(const UiSnapshot& snap) {
    Clear(kBlack);
    const int margin = 12;
    const char* state = "IDLE";
    uint16_t state_dot = kDarkGray;
    if (snap.recording) {
      state = "REC";
      state_dot = kRed;
    } else if (snap.monitoring) {
      state = "MON";
      state_dot = kGreen;
    }
    FillRect(126, 6, 16, 16, state_dot);
    DrawText(146, 6, state, kWhite, 3);

    DrawText(margin, 22, "ELAP", kCyan, 2);
    const std::string elapsed = FormatHms(snap.elapsed_sec);
    DrawText(margin, 43, elapsed, snap.finalize_pending ? kRed : kYellow, 4);
    if (snap.external_storage) {
      const int elapsed_w = static_cast<int>(elapsed.size()) * 6 * 4;
      DrawText(margin + elapsed_w + 8, 49, "E", kOrange, 2);
    }

    DrawText(margin, 86, "PEAK", kCyan, 2);
    const int meter_x = margin + 54;
    const int meter_y = 90;
    const int meter_w = 164;
    const int meter_h = 10;
    FillRect(meter_x - 1, meter_y - 1, meter_w + 2, meter_h + 2, kDarkGray);
    const int fill_w =
        (std::max(0, std::min(100, snap.peak_pct)) * meter_w) / 100;
    uint16_t meter_color = kGreen;
    if (snap.peak_pct >= 90) {
      meter_color = kRed;
    } else if (snap.peak_pct >= 70) {
      meter_color = kOrange;
    }
    if (fill_w > 0) {
      FillRect(meter_x, meter_y, fill_w, meter_h, meter_color);
    }
    if (fill_w < meter_w) {
      FillRect(meter_x + fill_w, meter_y, meter_w - fill_w, meter_h, kBlack);
    }

    DrawText(margin, 122, "MIC", kCyan, 2);
    DrawText(margin + 66, 122, snap.mic, snap.mic_connected ? kWhite : kRed, 2);

    char rate_ch[32];
    if (snap.zylia_gain_valid) {
      std::snprintf(rate_ch, sizeof(rate_ch), "%ukHz  CH:%d  G:%+ddB",
                    snap.rate / 1000, snap.channels, snap.zylia_gain_db);
    } else {
      std::snprintf(rate_ch, sizeof(rate_ch), "%ukHz  CH:%d", snap.rate / 1000,
                    snap.channels);
    }
    DrawText(margin, 146, rate_ch, kOrange, 2);

    char xr[32];
    std::snprintf(xr, sizeof(xr), "XRUN:%llu",
                  static_cast<unsigned long long>(snap.xruns));
    DrawText(margin, 176, xr, snap.xruns > 0 ? kRed : kWhite, 2);

    char dr[32];
    std::snprintf(dr, sizeof(dr), "DROP:%lluMB",
                  static_cast<unsigned long long>(snap.dropped_bytes /
                                                  (1024 * 1024)));
    DrawText(margin, 198, dr, snap.dropped_bytes > 0 ? kRed : kWhite, 2);

    char bat[16];
    if (snap.battery_valid) {
      std::snprintf(bat, sizeof(bat), "BAT %3d%%", snap.battery_pct);
    } else {
      std::snprintf(bat, sizeof(bat), "BAT --%%");
    }
    const int bat_scale = 2;
    const int bat_w = static_cast<int>(std::strlen(bat)) * 6 * bat_scale;
    const int bat_x = kWidth - margin - bat_w;
    const int bat_y = kHeight - (7 * bat_scale) - 6;
    uint16_t bat_color = kDarkGray;
    if (snap.battery_valid) {
      bat_color = (snap.battery_pct <= 20) ? kRed : kWhite;
    }
    DrawText(bat_x, bat_y, bat, bat_color, bat_scale);

    std::string rem = snap.storage_valid ? FormatHms(snap.remaining_storage_sec)
                                         : "--:--:--";
    uint16_t rem_color = kDarkGray;
    if (snap.storage_valid) {
      if (snap.remaining_storage_sec <= 600) {
        rem_color = kRed;
      } else if (snap.remaining_storage_sec <= 1800) {
        rem_color = kOrange;
      } else {
        rem_color = kGreen;
      }
    }
    DrawText(margin, bat_y, rem, rem_color, bat_scale);

    // DrawText(margin, 184, "KEY1:STOP KEY2:REC KEY3:BL", kDarkGray, 1);
    // DrawText(margin, 202, "L SPC R ZYL  U96 D48", kDarkGray, 1);
    return Flush();
  }

 private:
  bool Fail(const char* step) {
    last_error_ = step;
    return false;
  }

  bool FailErrno(const std::string& step) {
    const int e = errno;
    std::ostringstream oss;
    oss << step << " failed: errno=" << e << " (" << std::strerror(e) << ")";
    last_error_ = oss.str();
    return false;
  }

  static constexpr int kWidth = 240;
  static constexpr int kHeight = 240;
  static constexpr int kPinRst = 27;
  static constexpr int kPinDc = 25;
  static constexpr int kPinBl = 24;
  static constexpr int kPinKey1 = 21;
  static constexpr int kPinKey2 = 20;
  static constexpr int kPinKey3 = 16;
  static constexpr int kPinUp = 6;
  static constexpr int kPinDown = 19;
  static constexpr int kPinLeft = 5;
  static constexpr int kPinRight = 26;
  static constexpr int kPinPress = 13;

  static constexpr uint16_t kBlack = 0x0000;
  static constexpr uint16_t kWhite = 0xFFFF;
  static constexpr uint16_t kRed = 0xF800;
  static constexpr uint16_t kGreen = 0x07E0;
  static constexpr uint16_t kYellow = 0xFFE0;
  static constexpr uint16_t kCyan = 0x07FF;
  static constexpr uint16_t kDarkGray = 0x39E7;
  static constexpr uint16_t kOrange = 0xFD20;

  bool WriteCmd(uint8_t cmd, const uint8_t* data, size_t data_len) {
    if (!WriteGpio(dc_, 0) || !SpiWrite(&cmd, 1)) {
      return false;
    }
    if (data_len == 0) {
      return true;
    }
    return WriteGpio(dc_, 1) && SpiWrite(data, data_len);
  }

  bool SetWindow() {
    const uint8_t x[] = {0x00, 0x00, 0x00, 0xEF};
    const uint8_t y[] = {0x00, 0x00, 0x00, 0xEF};
    return WriteCmd(0x2A, x, sizeof(x)) && WriteCmd(0x2B, y, sizeof(y)) &&
           WriteCmd(0x2C, nullptr, 0);
  }

  bool Flush() {
    if (!SetWindow()) {
      return false;
    }
    if (!WriteGpio(dc_, 1)) {
      return false;
    }
    return SpiWrite(frame_.data(), frame_.size());
  }

  void Clear(uint16_t color) {
    const uint8_t hi = static_cast<uint8_t>(color >> 8);
    const uint8_t lo = static_cast<uint8_t>(color & 0xFF);
    for (size_t i = 0; i < frame_.size(); i += 2) {
      frame_[i] = hi;
      frame_[i + 1] = lo;
    }
  }

  void SetPixel(int x, int y, uint16_t color) {
    if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return;
    // Rotate full UI 90 degrees clockwise in software.
    const int rx = (kWidth - 1) - y;
    const int ry = x;
    const size_t idx = static_cast<size_t>(ry * kWidth + rx) * 2;
    frame_[idx] = static_cast<uint8_t>(color >> 8);
    frame_[idx + 1] = static_cast<uint8_t>(color & 0xFF);
  }

  void FillRect(int x, int y, int w, int h, uint16_t color) {
    for (int yy = y; yy < y + h; ++yy) {
      for (int xx = x; xx < x + w; ++xx) {
        SetPixel(xx, yy, color);
      }
    }
  }

  static const uint8_t* Glyph(char c) {
    static const uint8_t sp[] = {0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t dash[] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t colon[] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t dot[] = {0x00, 0x60, 0x60, 0x00, 0x00};
    static const uint8_t p0[] = {0x3E, 0x51, 0x49, 0x45, 0x3E};
    static const uint8_t p1[] = {0x00, 0x42, 0x7F, 0x40, 0x00};
    static const uint8_t p2[] = {0x42, 0x61, 0x51, 0x49, 0x46};
    static const uint8_t p3[] = {0x21, 0x41, 0x45, 0x4B, 0x31};
    static const uint8_t p4[] = {0x18, 0x14, 0x12, 0x7F, 0x10};
    static const uint8_t p5[] = {0x27, 0x45, 0x45, 0x45, 0x39};
    static const uint8_t p6[] = {0x3C, 0x4A, 0x49, 0x49, 0x30};
    static const uint8_t p7[] = {0x01, 0x71, 0x09, 0x05, 0x03};
    static const uint8_t p8[] = {0x36, 0x49, 0x49, 0x49, 0x36};
    static const uint8_t p9[] = {0x06, 0x49, 0x49, 0x29, 0x1E};
    static const uint8_t a[] = {0x7E, 0x11, 0x11, 0x11, 0x7E};
    static const uint8_t b[] = {0x7F, 0x49, 0x49, 0x49, 0x36};
    static const uint8_t c2[] = {0x3E, 0x41, 0x41, 0x41, 0x22};
    static const uint8_t d[] = {0x7F, 0x41, 0x41, 0x22, 0x1C};
    static const uint8_t e[] = {0x7F, 0x49, 0x49, 0x49, 0x41};
    static const uint8_t f[] = {0x7F, 0x09, 0x09, 0x09, 0x01};
    static const uint8_t g[] = {0x3E, 0x41, 0x49, 0x49, 0x7A};
    static const uint8_t h[] = {0x7F, 0x08, 0x08, 0x08, 0x7F};
    static const uint8_t i[] = {0x00, 0x41, 0x7F, 0x41, 0x00};
    static const uint8_t k[] = {0x7F, 0x08, 0x14, 0x22, 0x41};
    static const uint8_t l[] = {0x7F, 0x40, 0x40, 0x40, 0x40};
    static const uint8_t m[] = {0x7F, 0x02, 0x0C, 0x02, 0x7F};
    static const uint8_t n[] = {0x7F, 0x04, 0x08, 0x10, 0x7F};
    static const uint8_t o[] = {0x3E, 0x41, 0x41, 0x41, 0x3E};
    static const uint8_t p[] = {0x7F, 0x09, 0x09, 0x09, 0x06};
    static const uint8_t r[] = {0x7F, 0x09, 0x19, 0x29, 0x46};
    static const uint8_t s[] = {0x46, 0x49, 0x49, 0x49, 0x31};
    static const uint8_t t[] = {0x01, 0x01, 0x7F, 0x01, 0x01};
    static const uint8_t u[] = {0x3F, 0x40, 0x40, 0x40, 0x3F};
    static const uint8_t w[] = {0x7F, 0x20, 0x18, 0x20, 0x7F};
    static const uint8_t x[] = {0x63, 0x14, 0x08, 0x14, 0x63};
    static const uint8_t y[] = {0x03, 0x04, 0x78, 0x04, 0x03};
    static const uint8_t z[] = {0x61, 0x51, 0x49, 0x45, 0x43};

    switch (c) {
      case '0': return p0;
      case '1': return p1;
      case '2': return p2;
      case '3': return p3;
      case '4': return p4;
      case '5': return p5;
      case '6': return p6;
      case '7': return p7;
      case '8': return p8;
      case '9': return p9;
      case 'A': return a;
      case 'B': return b;
      case 'C': return c2;
      case 'D': return d;
      case 'E': return e;
      case 'F': return f;
      case 'G': return g;
      case 'H': return h;
      case 'I': return i;
      case 'K': return k;
      case 'L': return l;
      case 'M': return m;
      case 'N': return n;
      case 'O': return o;
      case 'P': return p;
      case 'R': return r;
      case 'S': return s;
      case 'T': return t;
      case 'U': return u;
      case 'W': return w;
      case 'X': return x;
      case 'Y': return y;
      case 'Z': return z;
      case ':': return colon;
      case '.': return dot;
      case '-': return dash;
      case ' ': return sp;
      default: return sp;
    }
  }

  void DrawText(int x, int y, const std::string& text, uint16_t color,
                int scale) {
    int cx = x;
    for (char raw : text) {
      char c = raw;
      if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
      }
      const uint8_t* g = Glyph(c);
      for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
          if ((g[col] >> row) & 0x01) {
            FillRect(cx + col * scale, y + row * scale, scale, scale, color);
          }
        }
      }
      cx += 6 * scale;
    }
  }

  struct GpioLine {
    unsigned int offset = 0;
    gpiod_line_request* request = nullptr;
  };

  bool RequestLine(GpioLine* line, unsigned int offset, bool output,
                   int initial_value, const char* label) {
    gpiod_line_settings* settings = gpiod_line_settings_new();
    if (!settings) {
      return FailErrno((std::string("gpiod_line_settings_new(") + label + ")")
                           .c_str());
    }
    gpiod_line_settings_set_direction(
        settings, output ? GPIOD_LINE_DIRECTION_OUTPUT
                         : GPIOD_LINE_DIRECTION_INPUT);
    if (output) {
      gpiod_line_settings_set_output_value(
          settings, initial_value ? GPIOD_LINE_VALUE_ACTIVE
                                  : GPIOD_LINE_VALUE_INACTIVE);
    } else {
      // Waveshare key/joystick lines are switch-to-GND inputs; request pull-up.
      (void)gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);
    }

    gpiod_line_config* line_cfg = gpiod_line_config_new();
    if (!line_cfg) {
      gpiod_line_settings_free(settings);
      return FailErrno((std::string("gpiod_line_config_new(") + label + ")")
                           .c_str());
    }
    const unsigned int offsets[1] = {offset};
    if (gpiod_line_config_add_line_settings(line_cfg, offsets, 1, settings) <
        0) {
      gpiod_line_config_free(line_cfg);
      gpiod_line_settings_free(settings);
      return FailErrno((std::string("gpiod_line_config_add_line_settings(") +
                        label + ")")
                           .c_str());
    }

    gpiod_request_config* req_cfg = gpiod_request_config_new();
    if (!req_cfg) {
      gpiod_line_config_free(line_cfg);
      gpiod_line_settings_free(settings);
      return FailErrno((std::string("gpiod_request_config_new(") + label + ")")
                           .c_str());
    }
    gpiod_request_config_set_consumer(req_cfg, "rpi_multirec");

    gpiod_line_request* req = gpiod_chip_request_lines(chip_, req_cfg, line_cfg);
    gpiod_request_config_free(req_cfg);
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);
    if (!req) {
      return FailErrno((std::string("gpiod_chip_request_lines(") + label + ")")
                           .c_str());
    }

    line->offset = offset;
    line->request = req;
    return true;
  }

  static void ReleaseLine(GpioLine& line) {
    if (line.request) {
      gpiod_line_request_release(line.request);
      line.request = nullptr;
    }
  }

  static bool WriteGpio(const GpioLine& line, int value) {
    if (!line.request) return false;
    const enum gpiod_line_value v =
        value ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
    return gpiod_line_request_set_value(
               line.request, line.offset,
               static_cast<enum gpiod_line_value>(v)) == 0;
  }

  static int ReadGpio(const GpioLine& line) {
    if (!line.request) return -1;
    const int v = gpiod_line_request_get_value(line.request, line.offset);
    if (v < 0) {
      return -1;
    }
    return (v == GPIOD_LINE_VALUE_ACTIVE) ? 1 : 0;
  }

  bool SpiWrite(const uint8_t* data, size_t len) {
    if (spi_fd_ < 0) return false;
    // spidev has a per-transfer size limit (often 4096 bytes), so send in chunks.
    constexpr size_t kSpiChunkBytes = 4096;
    size_t offset = 0;
    while (offset < len) {
      const size_t chunk = std::min(kSpiChunkBytes, len - offset);
      spi_ioc_transfer tr{};
      tr.tx_buf = reinterpret_cast<unsigned long>(data + offset);
      tr.len = static_cast<uint32_t>(chunk);
      tr.speed_hz = 32000000;
      tr.bits_per_word = 8;
      if (ioctl(spi_fd_, SPI_IOC_MESSAGE(1), &tr) < 0) {
        return false;
      }
      offset += chunk;
    }
    return true;
  }

  int spi_fd_ = -1;
  gpiod_chip* chip_ = nullptr;
  GpioLine dc_;
  GpioLine rst_;
  GpioLine bl_;
  GpioLine btn_[8];
  int last_btn_[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  int idle_btn_[8] = {1, 1, 1, 1, 1, 1, 1, 1};
  std::chrono::steady_clock::time_point next_repeat_[8];
  std::chrono::steady_clock::time_point key3_press_started_;
  bool key3_poweroff_fired_ = false;
  bool backlight_on_ = true;
  std::string last_error_;
  std::vector<uint8_t> frame_ =
      std::vector<uint8_t>(static_cast<size_t>(kWidth * kHeight * 2), 0);
};

#else

struct UiSnapshot {
  bool recording = false;
  bool monitoring = false;
  bool external_storage = false;
  std::string mic;
  bool mic_connected = true;
  bool battery_valid = false;
  int battery_pct = 0;
  bool storage_valid = false;
  uint64_t remaining_storage_sec = 0;
  bool finalize_pending = false;
  unsigned int rate = 0;
  int channels = 0;
  bool zylia_gain_valid = false;
  int zylia_gain_db = 0;
  int peak_pct = 0;
  uint64_t xruns = 0;
  uint64_t dropped_bytes = 0;
  int ring_fill_pct = 0;
  uint64_t elapsed_sec = 0;
};

class WaveshareHatUi {
 public:
  bool Init() { return false; }
  const std::string& LastError() const { return last_error_; }
  void Shutdown() {}
  void PollButtons(bool* start, bool* stop, bool* mic_left, bool* mic_right,
                   bool* rate_up, bool* rate_down, bool* backlight_toggle,
                   bool* poweroff, bool repeat_ud = false) {
    (void)repeat_ud;
    if (start) *start = false;
    if (stop) *stop = false;
    if (mic_left) *mic_left = false;
    if (mic_right) *mic_right = false;
    if (rate_up) *rate_up = false;
    if (rate_down) *rate_down = false;
    if (backlight_toggle) *backlight_toggle = false;
    if (poweroff) *poweroff = false;
  }
  void ToggleBacklight() {}
  bool ShowPoweroffMessage() { return false; }
  bool Render(const UiSnapshot&) { return false; }
 private:
  std::string last_error_ = "Not supported on non-Linux build";
};

#endif

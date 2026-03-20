#pragma once

#include "common.h"

struct UpsBatteryStatus {
  bool valid = false;
  int percent = 0;
  double bus_voltage_v = 0.0;
};

class UpsHatBMonitor {
 public:
  ~UpsHatBMonitor() { Close(); }

  bool Open(int i2c_bus = 1, uint8_t addr = 0x42) {
#ifdef __linux__
    Close();
    char dev[32];
    std::snprintf(dev, sizeof(dev), "/dev/i2c-%d", i2c_bus);
    fd_ = open(dev, O_RDWR);
    if (fd_ < 0) {
      return false;
    }
    if (ioctl(fd_, I2C_SLAVE, addr) < 0) {
      Close();
      return false;
    }
    if (!WriteReg16(0x05, 4096)) {
      Close();
      return false;
    }
    return true;
#else
    (void)i2c_bus;
    (void)addr;
    return false;
#endif
  }

  void Close() {
#ifdef __linux__
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
#endif
  }

  bool IsOpen() const {
#ifdef __linux__
    return fd_ >= 0;
#else
    return false;
#endif
  }

  bool Read(UpsBatteryStatus* out) {
    if (!out || !IsOpen()) {
      return false;
    }
#ifdef __linux__
    uint16_t raw = 0;
    if (!WriteReg16(0x05, 4096) || !ReadReg16(0x02, &raw)) {
      return false;
    }
    const double bus_v = static_cast<double>((raw >> 3) & 0x1FFF) * 0.004;
    const double p = ((bus_v - 6.0) / 2.4) * 100.0;
    out->bus_voltage_v = bus_v;
    out->percent =
        static_cast<int>(std::max(0.0, std::min(100.0, p)));
    out->valid = true;
    return true;
#else
    return false;
#endif
  }

 private:
#ifdef __linux__
  bool WriteReg16(uint8_t reg, uint16_t value) {
    if (fd_ < 0) {
      return false;
    }
    uint8_t data[3];
    data[0] = reg;
    data[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[2] = static_cast<uint8_t>(value & 0xFF);
    return write(fd_, data, 3) == 3;
  }

  bool ReadReg16(uint8_t reg, uint16_t* out) {
    if (fd_ < 0 || !out) {
      return false;
    }
    if (write(fd_, &reg, 1) != 1) {
      return false;
    }
    uint8_t data[2];
    if (read(fd_, data, 2) != 2) {
      return false;
    }
    *out = static_cast<uint16_t>((data[0] << 8) | data[1]);
    return true;
  }

  int fd_ = -1;
#endif
};

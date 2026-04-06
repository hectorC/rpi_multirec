#include "settings.h"

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

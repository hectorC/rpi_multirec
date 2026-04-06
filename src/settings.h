#pragma once

#include "common.h"

struct ManualClockState {
  int year = 2026;
  int month = 1;
  int day = 1;
  int hour = 0;
  int minute = 0;
  int second = 0;
};

bool IsLeapYear(int year);
int DaysInMonth(int year, int month);
void ClampManualClockState(ManualClockState* state);
std::string FormatManualClockDate(const ManualClockState& state);
std::string FormatManualClockTime(const ManualClockState& state);

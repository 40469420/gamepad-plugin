#pragma once

#include "hid-gyro.hpp"
#include "gamepad-runtime.hpp"

bool gamesir_hid_start();
void gamesir_hid_stop();
void gamesir_hid_set_capture(bool reports, bool tune, bool poll);
bool gamesir_hid_poll(GyroSample *out);
void gamesir_hid_copy_tune(StickTune *ls, StickTune *rs);
void gamesir_hid_copy_poll(int *set_hz, int *report_hz);

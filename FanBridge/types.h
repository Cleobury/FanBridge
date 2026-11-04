// Shared types (in a header so the Arduino IDE's auto-generated prototypes can see them).
#pragma once
#include <Arduino.h>
#include "fanlamp.h"

enum : uint8_t { F_LIGHT = 1, F_CT = 2, F_FAN = 4 };

struct Device {  // stored in flash
  char name[24];
  uint32_t id;
  uint8_t index, variant, speeds, flags, tx;
};

struct DevState {  // last commanded state (the fan never reports back)
  bool on;
  uint8_t bri;
  uint16_t mired;
  uint8_t fan;
  bool dir;
};

struct Job {  // one command waiting to be broadcast
  uint8_t dev, cmd, a0, a1, a2;
};

struct Learned {
  fanlamp::Decoded d;
  int8_t rssi;
};

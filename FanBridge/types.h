// Shared types (in a header so the Arduino IDE's auto-generated prototypes can see them).
#pragma once
#include <Arduino.h>
#include "fanlamp.h"

enum : uint8_t { F_LIGHT = 1, F_CT = 2, F_FAN = 4 };
enum : uint8_t { KIND_FAN = 0, KIND_BLIND = 1 };  // FanLamp fan/light, Tuiss blind

struct Device {  // stored in flash (store version 3)
  char name[24];
  uint32_t id;  // FanLamp remote ID
  uint8_t index, variant, speeds, flags, tx;
  uint8_t kind;    // KIND_*
  uint8_t model;   // Tuiss model index (tuiss::MODELS)
  uint8_t mac[6];  // Tuiss blind ID from the label
};

struct DeviceV2 {  // store version 2 layout, migrated on boot
  char name[24];
  uint32_t id;
  uint8_t index, variant, speeds, flags, tx;
};

struct DevState {  // last known state (fans never report back; blinds do when asked)
  bool on;
  uint8_t bri;
  uint16_t mired;
  uint8_t fan;
  bool dir;
  int8_t pos;      // blind: percent open, -1 = unknown
  int8_t target;   // blind: requested percent open, -1 = none
  uint8_t bat;     // blind: tuiss::BAT_*
  char msg[48];    // blind: result of the last command
};

struct Job {  // one fan command waiting to be broadcast
  uint8_t dev, cmd, a0, a1, a2;
};

struct Learned {
  fanlamp::Decoded d;
  int8_t rssi;
};

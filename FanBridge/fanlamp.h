// FanLamp / LampSmart v2/v3 BLE advertising encoder + decoder.
// Port of fan_encode.py; verified byte-for-byte against captures of the real remote and
// against ha-ble-adv reference vectors for the FanLamp Pro app's pairing packets.
#pragma once
#include <Arduino.h>
#include "mbedtls/aes.h"

namespace fanlamp {

static const uint8_t XBOXES[128] = {
  0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
  0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
  0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
  0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
  0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
  0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
  0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
  0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16,
};

// Command bytes
enum : uint8_t {
  CMD_LIGHT_TOGGLE = 0x09,
  CMD_LIGHT_ON = 0x10,
  CMD_LIGHT_OFF = 0x11,
  CMD_DIRECTION = 0x15,  // a0: 0 = forward, 1 = reverse
  CMD_CT = 0x21,         // a0 = 0, a1 = cold 0-255, a2 = warm 0-255
  CMD_PAIR = 0x28,
  CMD_FAN = 0x31,        // a0 = 0x20 (6-speed) or 0 (3-speed), a1 = speed (0 = off)
  CMD_UNPAIR = 0x45,
};

// Packet "dialects". The fan accepts any of these once it knows the sender's ID.
struct Variant {
  const char *key;
  const char *label;
  uint8_t prefix[3];
  bool sign;
  uint16_t devType;
  uint8_t adType;
  uint8_t flags;
};
static const Variant VARIANTS[] = {
  {"remote", "Physical remote (cloned)", {0x10, 0x00, 0x56}, true, 0x0400, 0x16, 0x02},
  {"fl_v3", "FanLamp Pro app v3", {0x20, 0x80, 0x00}, true, 0x0400, 0x03, 0x19},
  {"fl_v2", "FanLamp Pro app v2", {0x10, 0x80, 0x00}, false, 0x0400, 0x03, 0x19},
  {"ls_v3", "LampSmart Pro app v3", {0x30, 0x80, 0x00}, true, 0x0100, 0x03, 0x19},
  {"ls_v2", "LampSmart Pro app v2", {0x10, 0x80, 0x00}, false, 0x0100, 0x03, 0x19},
};
static const uint8_t NUM_VARIANTS = sizeof(VARIANTS) / sizeof(VARIANTS[0]);
enum : uint8_t { VAR_REMOTE = 0, VAR_FL_V3 = 1, VAR_FL_V2 = 2, VAR_LS_V3 = 3, VAR_LS_V2 = 4 };

// CRC-16/CCITT (same as Python binascii.crc_hqx)
inline uint16_t crc16(const uint8_t *b, size_t n, uint16_t crc) {
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)b[i] << 8;
    for (int j = 0; j < 8; j++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

inline uint16_t sign(const uint8_t *buf16, uint8_t tx, uint16_t seed) {
  const uint8_t key[16] = {(uint8_t)(seed & 0xFF), (uint8_t)(seed >> 8), tx, 0x0D, 0xBF, 0xE6, 0x42, 0x68,
                           0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16};
  uint8_t out[16];
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key, 128);
  mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, buf16, out);
  mbedtls_aes_free(&ctx);
  uint16_t s = out[0] | (out[1] << 8);
  return s ? s : 0xFFFF;
}

inline void whiten(const uint8_t *in, uint8_t *out, int n, int seed8, int salt) {
  for (int i = 0; i < n; i++) out[i] = XBOXES[((seed8 + i + 9) & 0x1F) + salt] ^ seed8 ^ in[i];
}

// Builds the full 31-byte legacy advertising payload.
inline void encode(uint8_t variant, uint32_t id, uint8_t index, uint8_t cmd, uint8_t param, uint8_t a0, uint8_t a1,
                   uint8_t a2, uint8_t tx, uint16_t seed, uint8_t out[31]) {
  const Variant &v = VARIANTS[variant < NUM_VARIANTS ? variant : 0];
  uint8_t d[22] = {v.prefix[0], v.prefix[1], v.prefix[2], tx, (uint8_t)(v.devType & 0xFF), (uint8_t)(v.devType >> 8),
                   (uint8_t)id, (uint8_t)(id >> 8), (uint8_t)(id >> 16), (uint8_t)(id >> 24),
                   index, cmd, 0x00, param, a0, a1, a2,
                   0x00, 0x00, 0x00, (uint8_t)(seed & 0xFF), (uint8_t)(seed >> 8)};
  if (v.sign) {
    uint16_t s = sign(d + 1, tx, seed);
    d[17] = s & 0xFF;
    d[18] = s >> 8;
  }
  const uint8_t hdr[7] = {0x02, 0x01, v.flags, 0x1B, v.adType, 0xF0, 0x08};
  memcpy(out, hdr, 7);
  uint8_t *p = out + 7;
  p[0] = d[0];
  p[1] = d[1];
  whiten(d + 2, p + 2, 18, seed & 0xFF, (d[1] & 3) << 5);
  p[20] = d[20];
  p[21] = d[21];
  uint16_t c = crc16(p, 22, seed ^ 0xFFFF);
  p[22] = c & 0xFF;
  p[23] = c >> 8;
}

// Result of decoding a received advertisement (used by "Learn remote").
struct Decoded {
  uint8_t variant;
  uint32_t id;
  uint8_t index;
  uint8_t cmd;
  uint8_t tx;
};

// Scans raw advertising data for a FanLamp packet. Returns true if one decodes with a valid CRC
// and matches a known variant.
inline bool decode(const uint8_t *data, int len, Decoded &out) {
  for (int i = 0; i + 1 < len;) {
    int l = data[i];
    if (l == 0 || i + 1 + l > len) break;
    uint8_t t = data[i + 1];
    if ((t == 0x16 || t == 0x03) && l == 27 && data[i + 2] == 0xF0 && data[i + 3] == 0x08) {
      const uint8_t *s = data + i + 4;  // 24 bytes
      uint16_t seed = s[20] | (s[21] << 8);
      if (crc16(s, 22, seed ^ 0xFFFF) != (uint16_t)(s[22] | (s[23] << 8))) return false;
      uint8_t d[20];
      d[0] = s[0];
      d[1] = s[1];
      whiten(s + 2, d + 2, 18, seed & 0xFF, (s[1] & 3) << 5);
      uint16_t dev = d[4] | (d[5] << 8);
      for (uint8_t v = 0; v < NUM_VARIANTS; v++) {
        const Variant &V = VARIANTS[v];
        if (V.adType == t && V.devType == dev && memcmp(V.prefix, d, 3) == 0) {
          out.variant = v;
          out.tx = d[3];
          out.id = d[6] | (d[7] << 8) | ((uint32_t)d[8] << 16) | ((uint32_t)d[9] << 24);
          out.index = d[10];
          out.cmd = d[11];
          return true;
        }
      }
      return false;
    }
    i += l + 1;
  }
  return false;
}

// Checks the encoder against captured / reference packets.
inline bool selfTest() {
  struct T {
    uint8_t var;
    uint32_t id;
    uint8_t index, cmd, tx;
    uint16_t seed;
    const char *hex;
  };
  static const T tests[] = {
    {VAR_REMOTE, 0x034A5A9D, 0, 0x09, 188, 0x54C5, "0201021B16F0081000A26CC1067B5C9750C056C2D745272E6822B0C554D85A"},
    {VAR_REMOTE, 0x034A5A9D, 0, 0x09, 189, 0x6799, "0201021B16F00810005C02AFA2F30FE73F7C61E841A88C9D05B25A99671EF0"},
    {VAR_FL_V3, 0xD2135C22, 2, 0x28, 117, 0x2B53, "0201191B03F0082080B801E122C6F2D3A76744A49F67F6B678458B532BB91B"},
    {VAR_FL_V2, 0xD2135C22, 2, 0x28, 117, 0x2B53, "0201191B03F0081080B801E122C6F2D3A76744A49F67F6B6A2228B532BE1B5"},
  };
  for (const T &t : tests) {
    uint8_t out[31], want[31];
    for (int i = 0; i < 31; i++) {
      char b[3] = {t.hex[i * 2], t.hex[i * 2 + 1], 0};
      want[i] = strtoul(b, nullptr, 16);
    }
    encode(t.var, t.id, t.index, t.cmd, 0, 0, 0, 0, t.tx, t.seed, out);
    if (memcmp(out, want, 31) != 0) return false;
    Decoded d;
    if (!decode(out, 31, d) || d.id != t.id || d.variant != t.var || d.cmd != t.cmd) return false;
  }
  return true;
}

}  // namespace fanlamp

// Tuiss SmartView blind driver (TS3000, TS5200, TS5300, TS2600, TS2900, TS5001, TS5101).
//
// Unlike the fans, a Tuiss blind needs a real BLE connection for every command:
//   scan for its advert -> connect -> find characteristics -> subscribe -> keepalive -> timestamp
//   -> command -> wait for notifications -> disconnect.
// This is written against the raw NimBLE host API so it shares the Bluetooth stack with Matter
// (the NimBLE-Arduino library would try to start a second copy of the stack).
// Protocol details from TuissESP by Callum Wallace (MIT): https://github.com/ba1i95/TuissESP
#pragma once
#include <Arduino.h>
#include <time.h>
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"

namespace tuiss {

static const char *const MODELS[] = {"TS3000", "TS5200", "TS5300", "TS2600", "TS2900", "TS5001", "TS5101"};
static const uint8_t NUM_MODELS = sizeof(MODELS) / sizeof(MODELS[0]);
static const uint8_t MAX_BLINDS = 8;  // indexed by FanBridge device index

enum : uint8_t { BAT_UNKNOWN = 0, BAT_GOOD = 1, BAT_LOW = 2 };

static const uint32_t FIND_MS = 30000;     // how long to look for the blind's advert
static const uint32_t CONNECT_MS = 6000;
static const uint32_t FRAME_WAIT_MS = 3500;

// 00010405-0405-0607-0809-0a0b0c0d1910 (write) and 00010304-... (notify), little-endian
static const ble_uuid128_t WRITE_UUID =
  BLE_UUID128_INIT(0x10, 0x19, 0x0d, 0x0c, 0x0b, 0x0a, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x05, 0x04, 0x01, 0x00);
static const ble_uuid128_t NOTIFY_UUID =
  BLE_UUID128_INIT(0x10, 0x19, 0x0d, 0x0c, 0x0b, 0x0a, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x04, 0x03, 0x01, 0x00);
static const ble_uuid16_t CCCD_UUID = BLE_UUID16_INIT(BLE_GATT_DSC_CLT_CFG_UUID16);  // notification config
static const uint8_t KEEPALIVE[] = {0xFF, 0x03, 0x03, 0x03, 0x03, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78};

// ---------------- command frames ----------------
enum Cmd : uint8_t { C_SET_OPEN, C_STOP, C_GET_POS, C_GET_BAT };

inline int buildCommand(Cmd c, uint8_t openPct, uint8_t *out) {
  out[0] = 0xFF, out[1] = 0x78, out[2] = 0xEA, out[3] = 0x41;
  switch (c) {
    case C_SET_OPEN: {  // ff78ea41 bf03 + (percent open * 10) as uint16 little-endian
      uint16_t raw = (uint16_t)min<uint8_t>(openPct, 100) * 10;
      out[4] = 0xBF, out[5] = 0x03, out[6] = raw & 0xFF, out[7] = raw >> 8;
      return 8;
    }
    case C_STOP: out[4] = 0x5F; break;
    case C_GET_POS: out[4] = 0xD1; break;
    case C_GET_BAT: out[4] = 0xF0; break;
  }
  out[5] = 0x03, out[6] = 0x01;
  return 7;
}

inline int monthIndex(const char *m) {
  static const char *names = "JanFebMarAprMayJunJulAugSepOctNovDec";
  for (int i = 0; i < 12; i++)
    if (!strncmp(m, names + i * 3, 3)) return i + 1;
  return 1;
}

// ff78ea41 0200 YY MM DD hh mm ss. Uses the clock if it has been set, otherwise the build time
// (TuissESP does the same; the blind only uses it for its own schedules).
inline int buildTimestamp(uint8_t *out) {
  int y, mo, d, h, mi, s;
  time_t now = time(nullptr);
  struct tm t;
  if (now > 1700000000 && localtime_r(&now, &t)) {
    y = t.tm_year + 1900, mo = t.tm_mon + 1, d = t.tm_mday, h = t.tm_hour, mi = t.tm_min, s = t.tm_sec;
  } else {
    const char *date = __DATE__, *tm_ = __TIME__;
    mo = monthIndex(date), d = atoi(date + 4), y = atoi(date + 7), h = atoi(tm_), mi = atoi(tm_ + 3), s = atoi(tm_ + 6);
  }
  const uint8_t f[12] = {0xFF, 0x78, 0xEA, 0x41, 0x02, 0x00, (uint8_t)constrain(y - 2000, 0, 255), (uint8_t)mo,
                         (uint8_t)d, (uint8_t)h, (uint8_t)mi, (uint8_t)s};
  memcpy(out, f, 12);
  return 12;
}

// Position frames carry percent open * 10 in bytes 7-8 (little-endian).
inline bool decodePosition(const uint8_t *f, int n, int8_t &openPct) {
  if (n < 9) return false;
  uint16_t raw = f[7] | (f[8] << 8);
  openPct = (int8_t)constrain((raw + 5) / 10, 0, 100);
  return true;
}
inline bool decodeBattery(const uint8_t *f, int n, uint8_t &bat) {
  if (n < 6 || f[4] != 0xD2) return false;
  bat = (n == 7 || f[5] >= 10) ? BAT_LOW : BAT_GOOD;
  return true;
}

// ---------------- advert helpers ----------------
inline bool parseMac(const char *s, uint8_t out[6]) {  // "AA:BB:CC:DD:EE:FF", "AA-BB-..." or "AABBCCDDEEFF"
  int n = 0;
  while (*s && n < 12) {
    char c = *s++;
    if (c == ':' || c == '-' || c == ' ') continue;
    if (!isxdigit((unsigned char)c)) return false;
    uint8_t v = isdigit((unsigned char)c) ? c - '0' : (toupper(c) - 'A' + 10);
    out[n / 2] = (n & 1) ? (out[n / 2] | v) : (v << 4);
    n++;
  }
  return n == 12 && !*s;
}

// Finds an AD structure of the given type in raw advertising data.
inline const uint8_t *findAd(const uint8_t *d, int len, uint8_t type, int &outLen) {
  for (int i = 0; i + 1 < len;) {
    int l = d[i];
    if (l == 0 || i + 1 + l > len) break;
    if (d[i + 1] == type) {
      outLen = l - 1;
      return d + i + 2;
    }
    i += l + 1;
  }
  return nullptr;
}

// The blind ID printed on the label is a MAC address that appears inside the manufacturer data
// (not necessarily the live BLE address). Accept it in either byte order.
inline bool advHasBlindId(const uint8_t *d, int len, const uint8_t id[6]) {
  int ml;
  const uint8_t *m = findAd(d, len, 0xFF, ml);
  if (!m || ml < 6) return false;
  for (int i = 0; i + 6 <= ml; i++) {
    bool fwd = true, rev = true;
    for (int k = 0; k < 6; k++) {
      fwd &= m[i + k] == id[k];
      rev &= m[i + k] == id[5 - k];
    }
    if (fwd || rev) return true;
  }
  return false;
}

// ---------------- "blinds nearby" list (for the web page) ----------------
struct Seen {
  ble_addr_t addr;
  char name[12];
  uint8_t mfr[24];
  uint8_t mfrLen;
  int8_t rssi;
  uint32_t lastMs;
};
static Seen seen[8];
static uint8_t numSeen = 0;
static portMUX_TYPE seenMux = portMUX_INITIALIZER_UNLOCKED;

inline void trackSeen(const struct ble_gap_disc_desc &d) {
  int nl = 0, ml = 0;
  const uint8_t *nm = findAd(d.data, d.length_data, 0x09, nl);
  if (!nm) nm = findAd(d.data, d.length_data, 0x08, nl);
  const uint8_t *mf = findAd(d.data, d.length_data, 0xFF, ml);
  portENTER_CRITICAL(&seenMux);
  int idx = -1;
  for (int i = 0; i < numSeen; i++)
    if (!memcmp(&seen[i].addr, &d.addr, sizeof(ble_addr_t))) idx = i;
  bool isTuiss = nm && nl >= 6 && nm[0] == 'T' && nm[1] == 'S' && isdigit(nm[2]);
  if (idx < 0 && isTuiss && numSeen < 8) {
    idx = numSeen++;
    memset(&seen[idx], 0, sizeof(Seen));
    seen[idx].addr = d.addr;
  }
  if (idx >= 0) {
    if (nm) {
      int n = min(nl, (int)sizeof(seen[idx].name) - 1);
      memcpy(seen[idx].name, nm, n);
      seen[idx].name[n] = 0;
    }
    if (mf && ml > 0) {
      seen[idx].mfrLen = min(ml, (int)sizeof(seen[idx].mfr));
      memcpy(seen[idx].mfr, mf, seen[idx].mfrLen);
    }
    seen[idx].rssi = d.rssi;
    seen[idx].lastMs = millis();
  }
  portEXIT_CRITICAL(&seenMux);
}

inline uint8_t copySeen(Seen *out) {
  portENTER_CRITICAL(&seenMux);
  uint8_t n = numSeen;
  memcpy(out, seen, sizeof(Seen) * n);
  portEXIT_CRITICAL(&seenMux);
  return n;
}

// ---------------- worker ----------------
enum : uint8_t { W_STOP = 1, W_SET = 2, W_REFRESH = 4 };
struct Pending {
  uint8_t want;
  uint8_t openPct;
  uint8_t mac[6];
};
struct Result {
  uint8_t dev;
  uint8_t want;      // which request this answers
  bool ok;
  int8_t pos;        // percent open, -1 if not read
  uint8_t bat;       // BAT_*
  uint8_t openPct;   // requested position (for W_SET)
  char msg[48];
};
typedef void (*ResultFn)(const Result &);
typedef bool (*ReadyFn)();

static Pending pend[MAX_BLINDS];
static portMUX_TYPE pendMux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t workSem, evtSem;
static volatile int evtStatus;
static volatile bool radioBusy_ = false;
static volatile int busyDev = -1;
static ResultFn resultFn;
static ReadyFn readyFn;
static ble_gap_event_fn *scanCb;  // FanBridge's shared scan handler (it forwards adverts to onAdvert)

// session state
static volatile bool finding = false, found = false;
static uint8_t findMac[6];
static ble_addr_t foundAddr;
static volatile uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t wrHandle, ntHandle, ntDef, cccd;
static uint16_t chrDefs[24];
static uint8_t numChrDefs;
static uint8_t frame[24];
static volatile uint8_t frameLen;
static volatile uint32_t frameSeq;

inline bool radioBusy() { return radioBusy_; }
inline int busyDevice() { return busyDev; }

inline void signal(int status) {
  evtStatus = status;
  xSemaphoreGive(evtSem);
}
inline void drain() {
  while (xSemaphoreTake(evtSem, 0) == pdTRUE) {}
}
// Waits for the callback started by `rc`'s call. Returns 0 on success, else an error code.
inline int await(int rc, uint32_t ms) {
  if (rc) return rc;
  if (xSemaphoreTake(evtSem, pdMS_TO_TICKS(ms)) != pdTRUE) return BLE_HS_ETIMEOUT;
  return evtStatus;
}

// Called by FanBridge for every scan result.
inline void onAdvert(const struct ble_gap_disc_desc &d) {
  trackSeen(d);
  if (finding && !found && advHasBlindId(d.data, d.length_data, findMac)) {
    foundAddr = d.addr;
    found = true;
    signal(0);
  }
}

static int connEvent(struct ble_gap_event *e, void *) {
  switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (e->connect.status == 0) {
        conn = e->connect.conn_handle;
        signal(0);
      } else {
        conn = BLE_HS_CONN_HANDLE_NONE;
        signal(e->connect.status);
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      conn = BLE_HS_CONN_HANDLE_NONE;
      signal(BLE_HS_ENOTCONN);
      break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
      uint16_t n = 0;
      uint8_t buf[sizeof(frame)];
      if (ble_hs_mbuf_to_flat(e->notify_rx.om, buf, sizeof(buf), &n) == 0 || n) {
        memcpy(frame, buf, n);
        frameLen = n;
        frameSeq++;
        Serial.print("Blind notify: ");
        for (int i = 0; i < n; i++) Serial.printf("%02X", buf[i]);
        Serial.println();
      }
      break;
    }
    default: break;
  }
  return 0;
}

static int chrFound(uint16_t, const struct ble_gatt_error *err, const struct ble_gatt_chr *c, void *) {
  if (err->status == 0 && c) {
    if (numChrDefs < 24) chrDefs[numChrDefs++] = c->def_handle;
    if (ble_uuid_cmp(&c->uuid.u, &WRITE_UUID.u) == 0) wrHandle = c->val_handle;
    if (ble_uuid_cmp(&c->uuid.u, &NOTIFY_UUID.u) == 0) ntHandle = c->val_handle, ntDef = c->def_handle;
  } else {
    signal(err->status == BLE_HS_EDONE ? 0 : err->status);
  }
  return 0;
}

static int dscFound(uint16_t, const struct ble_gatt_error *err, uint16_t, const struct ble_gatt_dsc *d, void *) {
  if (err->status == 0 && d) {
    if (ble_uuid_cmp(&d->uuid.u, &CCCD_UUID.u) == 0) cccd = d->handle;
  } else {
    signal(err->status == BLE_HS_EDONE ? 0 : err->status);
  }
  return 0;
}

static int written(uint16_t, const struct ble_gatt_error *err, struct ble_gatt_attr *, void *) {
  signal(err->status);
  return 0;
}

inline int writeChr(uint16_t handle, const uint8_t *data, int len) {
  drain();
  return await(ble_gattc_write_flat(conn, handle, data, len, written, NULL), 3000);
}

inline void disconnect() {
  if (conn == BLE_HS_CONN_HANDLE_NONE) return;
  drain();
  if (ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM) == 0) {
    uint32_t t0 = millis();
    while (conn != BLE_HS_CONN_HANDLE_NONE && millis() - t0 < 3000) delay(20);
  }
  conn = BLE_HS_CONN_HANDLE_NONE;
}

// Runs one connection and sends `cmds` in order. Fills r.pos / r.bat from any replies.
inline bool session(const uint8_t mac[6], const Cmd *cmds, int numCmds, uint8_t openPct, Result &r) {
  // 1. find the blind's advert (shares the scan with FanBridge's "Learn")
  drain();
  memcpy(findMac, mac, 6);
  found = false;
  finding = true;
  uint32_t t0 = millis();
  while (!found && millis() - t0 < FIND_MS) {
    if (!ble_gap_disc_active()) {
      struct ble_gap_disc_params p = {};
      p.passive = 0;  // active: the model name is in the scan response
      p.itvl = 0x50;
      p.window = 0x30;
      ble_gap_disc(BLE_OWN_ADDR_PUBLIC, FIND_MS, &p, scanCb, NULL);
    }
    xSemaphoreTake(evtSem, pdMS_TO_TICKS(250));
  }
  finding = false;
  if (!found) {
    snprintf(r.msg, sizeof(r.msg), "Blind not found - out of range?");
    return false;
  }

  // 2. take the radio: stop scanning, let any fan broadcast finish, connect
  radioBusy_ = true;
  ble_gap_disc_cancel();
  for (uint32_t t = millis(); ble_gap_adv_active() && millis() - t < 1500;) delay(20);
  if (ble_gap_adv_active()) ble_gap_adv_stop();

  int rc = -1;
  for (int attempt = 0; attempt < 2 && rc != 0; attempt++) {
    if (attempt) {
      ble_gap_conn_cancel();
      delay(300);
    }
    drain();
    rc = await(ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &foundAddr, CONNECT_MS, NULL, connEvent, NULL), CONNECT_MS + 1500);
  }
  if (rc != 0 || conn == BLE_HS_CONN_HANDLE_NONE) {
    ble_gap_conn_cancel();
    snprintf(r.msg, sizeof(r.msg), "Connect failed (%d)", rc);
    radioBusy_ = false;
    return false;
  }

  bool ok = false;
  do {
    // 3. characteristics + notification descriptor
    wrHandle = ntHandle = ntDef = cccd = 0;
    numChrDefs = 0;
    drain();
    rc = await(ble_gattc_disc_all_chrs(conn, 1, 0xFFFF, chrFound, NULL), 6000);
    if (rc || !wrHandle || !ntHandle) {
      snprintf(r.msg, sizeof(r.msg), "Blind characteristics not found (%d)", rc);
      break;
    }
    uint16_t end = 0xFFFF;
    for (int i = 0; i < numChrDefs; i++)
      if (chrDefs[i] > ntDef && chrDefs[i] - 1 < end) end = chrDefs[i] - 1;
    drain();
    await(ble_gattc_disc_all_dscs(conn, ntHandle, end, dscFound, NULL), 4000);
    if (!cccd) cccd = ntHandle + 1;  // the usual layout

    // 4. subscribe, keepalive, timestamp
    const uint8_t on[2] = {0x01, 0x00};
    if ((rc = writeChr(cccd, on, 2))) {
      snprintf(r.msg, sizeof(r.msg), "Subscribe failed (%d)", rc);
      break;
    }
    if ((rc = writeChr(wrHandle, KEEPALIVE, sizeof(KEEPALIVE)))) {
      snprintf(r.msg, sizeof(r.msg), "Keepalive failed (%d)", rc);
      break;
    }
    uint8_t buf[16];
    int n = buildTimestamp(buf);
    if ((rc = writeChr(wrHandle, buf, n))) {
      snprintf(r.msg, sizeof(r.msg), "Timestamp failed (%d)", rc);
      break;
    }

    // 5. commands, each followed by a wait for the blind's notifications
    ok = true;
    for (int c = 0; c < numCmds && ok; c++) {
      uint32_t seq0 = frameSeq;
      n = buildCommand(cmds[c], openPct, buf);
      if ((rc = writeChr(wrHandle, buf, n))) {
        snprintf(r.msg, sizeof(r.msg), "Command failed (%d)", rc);
        ok = false;
        break;
      }
      bool wantReply = cmds[c] == C_GET_POS || cmds[c] == C_GET_BAT;
      bool got = false;
      uint32_t seqSeen = seq0;
      for (uint32_t t = millis(); millis() - t < FRAME_WAIT_MS && !got && conn != BLE_HS_CONN_HANDLE_NONE;) {
        if (frameSeq != seqSeen) {
          seqSeen = frameSeq;
          uint8_t f[sizeof(frame)];
          uint8_t fl = frameLen;
          memcpy(f, frame, fl);
          if (cmds[c] == C_GET_POS) got = decodePosition(f, fl, r.pos);
          else if (cmds[c] == C_GET_BAT) got = decodeBattery(f, fl, r.bat);
          else got = true;  // any reply means the blind took the command
        }
        delay(20);
      }
      (void)wantReply;
    }
  } while (false);

  disconnect();
  radioBusy_ = false;
  return ok;
}

static void workerTask(void *) {
  for (;;) {
    xSemaphoreTake(workSem, pdMS_TO_TICKS(1000));
    if (!readyFn || !readyFn()) continue;
    for (int i = 0; i < MAX_BLINDS; i++) {
      Pending p;
      portENTER_CRITICAL(&pendMux);
      p = pend[i];
      uint8_t take = (p.want & W_STOP) ? W_STOP : (p.want & W_SET) ? W_SET : (p.want & W_REFRESH) ? W_REFRESH : 0;
      pend[i].want &= ~take;
      portEXIT_CRITICAL(&pendMux);
      if (!take) continue;

      Result r = {};
      r.dev = i;
      r.want = take;
      r.pos = -1;
      r.openPct = p.openPct;
      busyDev = i;
      if (take == W_STOP) {
        const Cmd c[] = {C_STOP, C_GET_POS};
        r.ok = session(p.mac, c, 2, 0, r);
      } else if (take == W_SET) {
        const Cmd c[] = {C_SET_OPEN};
        r.ok = session(p.mac, c, 1, p.openPct, r);
      } else {
        const Cmd c[] = {C_GET_POS, C_GET_BAT};
        r.ok = session(p.mac, c, 2, 0, r);
      }
      if (r.ok && !r.msg[0]) snprintf(r.msg, sizeof(r.msg), "OK");
      busyDev = -1;
      if (resultFn) resultFn(r);
      xSemaphoreGive(workSem);  // look again: more may have been queued meanwhile
      break;
    }
  }
}

// scan: FanBridge's GAP handler for scans (must call tuiss::onAdvert for each BLE_GAP_EVENT_DISC).
inline void begin(ble_gap_event_fn *scan, ReadyFn ready, ResultFn result) {
  scanCb = scan;
  readyFn = ready;
  resultFn = result;
  workSem = xSemaphoreCreateBinary();
  evtSem = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(workerTask, "tuiss", 6144, NULL, 1, NULL, 1);
}

inline void request(int dev, uint8_t want, const uint8_t mac[6], uint8_t openPct = 0) {
  if (dev < 0 || dev >= MAX_BLINDS) return;
  portENTER_CRITICAL(&pendMux);
  Pending &p = pend[dev];
  memcpy(p.mac, mac, 6);
  if (want & W_STOP) p.want &= ~W_SET;  // stop cancels a queued move
  if (want & W_SET) p.want &= ~W_STOP, p.openPct = openPct;
  p.want |= want;
  portEXIT_CRITICAL(&pendMux);
  xSemaphoreGive(workSem);
}

inline bool pending(int dev) { return dev >= 0 && dev < MAX_BLINDS && pend[dev].want; }

inline void clearAll() {
  portENTER_CRITICAL(&pendMux);
  memset(pend, 0, sizeof(pend));
  portEXIT_CRITICAL(&pendMux);
}

}  // namespace tuiss

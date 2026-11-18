// FanBridge: control FanLamp-protocol ceiling fans/lights (e.g. Philips Bliss) and Tuiss SmartView blinds
// from a web page and Google Home.
// Board: XIAO ESP32-S3, ESP32 Arduino core 3.3.x.
//   Fans:   the ESP32 broadcasts the same BLE packets as the fans' remotes (fanlamp.h).
//   Blinds: the ESP32 connects to each blind over BLE for every command (tuiss.h).
//
// USE_MATTER 1 -> each device appears in Google Home (Matter). Wi-Fi comes from Google Home setup.
// USE_MATTER 0 -> web page + serial only. Set Wi-Fi from the Serial Monitor: wifi <ssid> <password>

#define USE_MATTER 1
#define MAX_DEVICES 8
#define MIREDS_COLD 153         // coolest white (6500K)
#define MIREDS_WARM 370         // warmest white (2700K)
#define BURST_MS 800            // how long each fan command is broadcast
#define LEARN_MS 20000          // how long "Learn" listens
#define BLIND_REFRESH_MIN 30    // re-read blind positions this often (0 = only at boot / on request)

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include "esp_netif.h"
#include "fanlamp.h"
#include "tuiss.h"
#include "types.h"
#include "web_ui.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"

#if USE_MATTER
#include <Matter.h>
#else
#include <BLEDevice.h>
#endif

using namespace fanlamp;

// ---------------- devices ----------------
static const uint8_t STORE_VERSION = 3;
Device devs[MAX_DEVICES];
DevState st[MAX_DEVICES];
uint8_t numDevs = 0;
bool rebootNeeded = false;  // device list differs from the Matter endpoints created at boot
bool selfTestOk = false;
Preferences prefs;
WebServer server(80);
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static DevState defaultState() {
  DevState s = {};
  s.bri = 255;
  s.mired = (MIREDS_COLD + MIREDS_WARM) / 2;
  s.pos = -1;
  s.target = -1;
  return s;
}

static bool isBlind(int i) { return i >= 0 && i < numDevs && devs[i].kind == KIND_BLIND; }

static void saveDevices() {
  prefs.putUChar("ver", STORE_VERSION);
  prefs.putUChar("n", numDevs);
  prefs.putBytes("devs", devs, sizeof(Device) * numDevs);
}

static void loadDevices() {
  uint8_t ver = prefs.getUChar("ver", 0);
  if (ver == STORE_VERSION) {
    numDevs = min<uint8_t>(prefs.getUChar("n", 0), MAX_DEVICES);
    if (numDevs) prefs.getBytes("devs", devs, sizeof(Device) * numDevs);
  } else if (ver == 2) {  // fans-only firmware: convert in place
    numDevs = min<uint8_t>(prefs.getUChar("n", 0), MAX_DEVICES);
    DeviceV2 old[MAX_DEVICES];
    if (numDevs) prefs.getBytes("devs", old, sizeof(DeviceV2) * numDevs);
    for (int i = 0; i < numDevs; i++) {
      devs[i] = {};
      memcpy(devs[i].name, old[i].name, sizeof(old[i].name));
      devs[i].id = old[i].id;
      devs[i].index = old[i].index;
      devs[i].variant = old[i].variant;
      devs[i].speeds = old[i].speeds;
      devs[i].flags = old[i].flags;
      devs[i].tx = old[i].tx;
      devs[i].kind = KIND_FAN;
    }
    if (numDevs) saveDevices();
    Serial.printf("Migrated %u device(s) from the previous firmware\r\n", numDevs);
  }
  if (numDevs == 0) {
    // First boot: one fan + light with a random ID, so Google Home has something to add.
    // On the web page, either "Learn" your remote and "Apply" it to this device, or press "Pair".
    Device d = {};
    strcpy(d.name, "Ceiling fan");
    do d.id = esp_random();
    while (!d.id);
    d.variant = VAR_FL_V3;
    d.speeds = 6;
    d.flags = F_LIGHT | F_CT | F_FAN;
    d.kind = KIND_FAN;
    devs[0] = d;
    numDevs = 1;
    saveDevices();
  }
  for (int i = 0; i < MAX_DEVICES; i++) st[i] = defaultState();
}

// ---------------- fan transmit queue (later commands of the same kind replace earlier ones) ----------------
static Job jobs[16];
static uint8_t numJobs = 0;

static int slotOf(uint8_t c) {
  switch (c) {
    case CMD_LIGHT_ON:
    case CMD_LIGHT_OFF: return 1;
    case CMD_CT: return 2;
    case CMD_FAN: return 3;
    case CMD_DIRECTION: return 4;
    default: return 0;  // toggle, pair, ... always sent
  }
}

static void enqueue(Job j) {
  portENTER_CRITICAL(&mux);
  int s = slotOf(j.cmd);
  bool done = false;
  for (int i = 0; s && i < numJobs && !done; i++)
    if (jobs[i].dev == j.dev && slotOf(jobs[i].cmd) == s) jobs[i] = j, done = true;
  if (!done && numJobs < 16) jobs[numJobs++] = j;
  portEXIT_CRITICAL(&mux);
}

static bool dequeue(Job &j) {
  portENTER_CRITICAL(&mux);
  bool got = numJobs > 0;
  if (got) {
    j = jobs[0];
    memmove(jobs, jobs + 1, sizeof(Job) * --numJobs);
  }
  portEXIT_CRITICAL(&mux);
  return got;
}

static void qLight(int i, bool on) { enqueue({(uint8_t)i, on ? CMD_LIGHT_ON : CMD_LIGHT_OFF, 0, 0, 0}); }
static void qCt(int i) {
  float t = constrain((float)(MIREDS_WARM - (int)st[i].mired) / (MIREDS_WARM - MIREDS_COLD), 0.0f, 1.0f);
  enqueue({(uint8_t)i, CMD_CT, 0, (uint8_t)lroundf(st[i].bri * t), (uint8_t)lroundf(st[i].bri * (1.0f - t))});
}
static void qFan(int i, uint8_t s) { enqueue({(uint8_t)i, CMD_FAN, (uint8_t)(devs[i].speeds == 6 ? 0x20 : 0x00), s, 0}); }

// ---------------- BLE ----------------
static Learned learned[8];
static uint8_t numLearned = 0;
static uint32_t learnEnd = 0;
static uint32_t burstEnd = 0;

static bool isLearning() { return learnEnd && (int32_t)(learnEnd - millis()) > 0; }

// Shared handler for scans (Learn and blind discovery) and fan broadcasts.
static int gapEvent(struct ble_gap_event *e, void *) {
  if (e->type == BLE_GAP_EVENT_DISC) {
    tuiss::onAdvert(e->disc);
    if (!isLearning()) return 0;  // fan remotes only matter while learning: skip the decode work otherwise
    Decoded d;
    if (decode(e->disc.data, e->disc.length_data, d)) {
      portENTER_CRITICAL(&mux);
      bool known = false;
      for (int i = 0; i < numLearned; i++)
        if (learned[i].d.id == d.id && learned[i].d.variant == d.variant && learned[i].d.index == d.index) {
          learned[i] = {d, e->disc.rssi};
          known = true;
        }
      if (!known && numLearned < 8) learned[numLearned++] = {d, e->disc.rssi};
      portEXIT_CRITICAL(&mux);
    }
  }
  return 0;
}

static bool bleReady() {
#if USE_MATTER
  if (!Matter.isDeviceCommissioned()) return false;  // Matter uses the radio while being added to Google Home
#endif
  return ble_hs_synced();
}

static void scanFor(uint32_t ms) {
  if (ble_gap_disc_active() || tuiss::radioBusy()) return;
  struct ble_gap_disc_params p = {};
  p.passive = 0;  // active, so Tuiss blinds' names (scan responses) show up too
  p.itvl = 0x80;    // 80 ms
  p.window = 0x20;  // listen 20 ms in every 80: Wi-Fi shares the antenna, so keep scanning light
  ble_gap_disc(BLE_OWN_ADDR_PUBLIC, ms, &p, gapEvent, NULL);
}

static bool startLearn() {
  if (!bleReady() || isLearning()) return false;
  portENTER_CRITICAL(&mux);
  numLearned = 0;
  portEXIT_CRITICAL(&mux);
  learnEnd = millis() + LEARN_MS;
  scanFor(LEARN_MS);
  Serial.println("Learn: listening for remotes and blinds");
  return true;
}

static void startBurst(const Job &j) {
  Device &d = devs[j.dev];
  d.tx++;
  saveDevices();
  uint16_t seed = 1 + (esp_random() % 0xFFF4);
  uint8_t adv[31];
  encode(d.variant, d.id, d.index, j.cmd, 0, j.a0, j.a1, j.a2, d.tx, seed, adv);
  if (ble_gap_adv_active()) ble_gap_adv_stop();
  int rc = ble_gap_adv_set_data(adv, sizeof(adv));
  if (rc == 0) {
    struct ble_gap_adv_params p = {};
    p.conn_mode = BLE_GAP_CONN_MODE_NON;
    p.disc_mode = BLE_GAP_DISC_MODE_NON;
    p.itvl_min = p.itvl_max = 0x20;  // 20 ms
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p, gapEvent, NULL);
  }
  Serial.printf("TX [%s] cmd=0x%02X a=%02X %02X %02X tx=%u -> %s (rc=%d)\r\n", d.name, j.cmd, j.a0, j.a1, j.a2, d.tx,
                rc == 0 ? "sent" : "FAILED", rc);
  burstEnd = millis() + BURST_MS;
}

static void serviceRadio() {
  if (burstEnd && (int32_t)(millis() - burstEnd) >= 0) {
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    burstEnd = 0;
    tuiss::fanDone();
  }
  if (burstEnd || !bleReady() || !tuiss::claimForFan()) return;  // a blind connection has (or wants) the radio
  Job j;
  if (dequeue(j) && j.dev < numDevs && devs[j.dev].kind == KIND_FAN) startBurst(j);
  if (!burstEnd) tuiss::fanDone();  // nothing sent
}

// Fan speed <-> percentage (6 speeds: 17, 33, 50, 67, 83, 100 %; 3 speeds: 33, 67, 100 %)
static uint8_t speedToPct(uint8_t s, uint8_t n) { return (s * 100 + n / 2) / n; }
static uint8_t pctToSpeed(uint8_t pct, uint8_t n) { return pct ? constrain((pct * n + 50) / 100, 1, n) : 0; }

// ---------------- blinds ----------------
volatile bool blindSyncPending[MAX_DEVICES];  // Google Home needs the blind's new position

static void blindRequest(int i, uint8_t want, uint8_t openPct = 0) {
  if (!isBlind(i)) return;
  if (want & tuiss::W_SET) st[i].target = openPct;
  if (want & tuiss::W_STOP) st[i].target = -1;
  snprintf(st[i].msg, sizeof(st[i].msg), "Working...");
  tuiss::request(i, want, devs[i].mac, openPct);
}

static void onBlindResult(const tuiss::Result &r) {  // runs on the blind worker task
  int i = r.dev;
  if (!isBlind(i)) return;
  if (r.ok && r.want == tuiss::W_SET) st[i].pos = r.openPct;
  if (r.pos >= 0) st[i].pos = r.pos;
  if (r.bat) st[i].bat = r.bat;
  if (r.want != tuiss::W_REFRESH && !tuiss::pending(i)) st[i].target = -1;
  strlcpy(st[i].msg, r.msg, sizeof(st[i].msg));
  Serial.printf("Blind [%s]: %s, %d%% open, battery %s\r\n", devs[i].name, r.msg, st[i].pos,
                st[i].bat == tuiss::BAT_LOW ? "LOW" : st[i].bat == tuiss::BAT_GOOD ? "good" : "?");
  blindSyncPending[i] = true;
}

static void refreshAllBlinds() {
  for (int i = 0; i < numDevs; i++)
    if (isBlind(i)) blindRequest(i, tuiss::W_REFRESH);
}

// ---------------- Matter ----------------
#if USE_MATTER
uint8_t bootFlags[MAX_DEVICES];  // which endpoints exist (fixed until reboot)
uint8_t bootKind[MAX_DEVICES];
uint8_t bootCount = 0;
static bool fromWeb = false;              // set while FanBridge itself updates Matter's state
uint8_t lastSpeed[MAX_DEVICES];           // last non-zero fan speed, restored on "turn on"
volatile bool snapPending[MAX_DEVICES];   // Google's fan state needs tidying to the real speed step
volatile bool stopSeen[MAX_DEVICES];      // Matter reports Stop and then a go-to; ignore that go-to

// Matter callbacks are tied to the device list at boot; ignore them once it has changed.
static bool matterLive(int i, const char *what, int v) {
  if (fromWeb) return false;
  bool ok = !rebootNeeded && i < bootCount && i < numDevs;
  Serial.printf("Google Home -> %s: %s = %d%s\r\n", i < numDevs ? devs[i].name : "?", what, v,
                ok ? "" : "  (IGNORED: device list changed, reboot FanBridge)");
  return ok;
}

static void fanFromGoogle(int i, uint8_t s) {
  if (i < 0 || i >= numDevs) return;
  st[i].fan = s;
  if (s) lastSpeed[i] = s;
  snapPending[i] = true;
  if (matterLive(i, "fan speed", s)) qFan(i, s);
}

static void fanModeFromGoogle(int i, uint8_t mode) {
  if (i < 0 || i >= numDevs) return;
  uint8_t n = devs[i].speeds, s;
  switch (mode) {
    case MatterFan::FAN_MODE_OFF: s = 0; break;
    case MatterFan::FAN_MODE_LOW: s = max(1, n / 3); break;
    case MatterFan::FAN_MODE_MEDIUM: s = (n + 1) / 2; break;
    case MatterFan::FAN_MODE_HIGH: s = n; break;
    default: s = st[i].fan ? st[i].fan : (lastSpeed[i] ? lastSpeed[i] : (n + 1) / 2); break;  // On / Auto / Smart
  }
  fanFromGoogle(i, s);
}

// MatterFan with its attribute handler replaced. The stock handler writes the percentage attributes
// again from inside the change callback, which re-enters itself forever (stack overflow). This one
// only records the request; loop() reports the tidied-up state back to Google afterwards.
class BridgeFan : public MatterFan {
public:
  int dev = -1;
  bool attributeChangeCB(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, esp_matter_attr_val_t *val) override {
    if (endpoint_id != getEndPointId() || cluster_id != chip::app::Clusters::FanControl::Id) return true;
    namespace FA = chip::app::Clusters::FanControl::Attributes;
    if (attribute_id == FA::FanMode::Id) {
      currentFanMode = (FanMode_t)val->val.u8;
      if (!fromWeb) fanModeFromGoogle(dev, val->val.u8);
    } else if (attribute_id == FA::PercentSetting::Id && val->val.u8 <= 100) {
      currentPercent = val->val.u8;
      if (!fromWeb) fanFromGoogle(dev, pctToSpeed(val->val.u8, devs[dev].speeds));
    }
    return true;
  }
};

BridgeFan fans[MAX_DEVICES];
MatterColorTemperatureLight ctLights[MAX_DEVICES];
MatterOnOffLight onoffLights[MAX_DEVICES];
MatterWindowCovering covers[MAX_DEVICES];

// Update a fan attribute for Google without running any attribute callbacks.
static void reportFanAttr(int i, uint32_t attr, uint8_t v) {
  uint16_t ep = fans[i].getEndPointId();
  uint32_t cl = chip::app::Clusters::FanControl::Id;
  esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);  // called from loop(), not the Matter task
  esp_matter_attr_val_t val = esp_matter_invalid(NULL);
  if (esp_matter::attribute::get_val(ep, cl, attr, &val) != ESP_OK) return;
  if (val.val.u8 == v) return;
  val.val.u8 = v;  // keeps the attribute's own type (uint8 / nullable uint8 / enum8)
  esp_matter::attribute::report(ep, cl, attr, &val);
}

// Make Google Home's fan mode + percentage match st[i].fan.
static void syncMatterFan(int i) {
  namespace FA = chip::app::Clusters::FanControl::Attributes;
  bool prev = fromWeb;
  fromWeb = true;
  uint8_t pct = speedToPct(st[i].fan, devs[i].speeds);
  reportFanAttr(i, FA::PercentSetting::Id, pct);
  reportFanAttr(i, FA::PercentCurrent::Id, pct);
  reportFanAttr(i, FA::FanMode::Id, st[i].fan ? MatterFan::FAN_MODE_ON : MatterFan::FAN_MODE_OFF);
  fromWeb = prev;
}

// Matter counts lift as percent CLOSED (0 = open); Tuiss counts percent OPEN.
static uint16_t closed100ths(int8_t openPct) { return (uint16_t)(100 - constrain(openPct, 0, 100)) * 100; }

static void syncMatterBlind(int i) {
  if (st[i].pos < 0) return;
  bool prev = fromWeb;
  fromWeb = true;
  covers[i].setCurrentLiftPercent100ths(closed100ths(st[i].pos));
  covers[i].setTargetLiftPercent100ths(closed100ths(st[i].target >= 0 ? st[i].target : st[i].pos));
  fromWeb = prev;
}

static void setupMatter() {
  bootCount = numDevs;
  for (int k = 0; k < numDevs; k++) {
    const int i = k;
    bootKind[i] = devs[i].kind;
    if (devs[i].kind == KIND_BLIND) {
      bootFlags[i] = 0;
      covers[i].begin(0, 0, MatterWindowCovering::ROLLERSHADE);
      covers[i].onStop([i]() {
        if (fromWeb) return true;
        stopSeen[i] = true;
        if (matterLive(i, "blind stop", 0)) blindRequest(i, tuiss::W_STOP);
        return true;
      });
      covers[i].onGoToLiftPercentage([i](uint8_t closedPct) {
        if (fromWeb) return true;
        if (stopSeen[i]) {
          stopSeen[i] = false;
          return true;
        }
        uint8_t open = 100 - min<uint8_t>(closedPct, 100);
        if (matterLive(i, "blind % open", open)) blindRequest(i, tuiss::W_SET, open);
        return true;
      });
      continue;
    }
    uint8_t f = bootFlags[i] = devs[i].flags;
    if ((f & F_LIGHT) && (f & F_CT)) {
      MatterColorTemperatureLight &L = ctLights[i];
      L.begin(false, 255, st[i].mired);
      L.onChangeOnOff([i](bool on) {
        st[i].on = on;
        if (matterLive(i, "light", on)) qLight(i, on);
        return true;
      });
      L.onChangeBrightness([i](uint8_t b) {
        st[i].bri = max<uint8_t>(b, 1);
        if (matterLive(i, "brightness", b) && st[i].on) qCt(i);
        return true;
      });
      L.onChangeColorTemperature([i](uint16_t m) {
        st[i].mired = m;
        if (matterLive(i, "colour temp (mireds)", m) && st[i].on) qCt(i);
        return true;
      });
    } else if (f & F_LIGHT) {
      onoffLights[i].begin(false);
      onoffLights[i].onChangeOnOff([i](bool on) {
        st[i].on = on;
        if (matterLive(i, "light", on)) qLight(i, on);
        return true;
      });
    }
    if (f & F_FAN) {
      fans[i].dev = i;
      fans[i].begin(0, MatterFan::FAN_MODE_OFF, MatterFan::FAN_MODE_SEQ_OFF_LOW_MED_HIGH);
    }
  }
  Matter.begin();
}

// Reflect a web-page change in Google Home.
static void syncMatter(int i) {
  if (rebootNeeded || i >= bootCount) return;
  if (bootKind[i] == KIND_BLIND) {
    syncMatterBlind(i);
    return;
  }
  fromWeb = true;
  uint8_t f = bootFlags[i];
  if ((f & F_LIGHT) && (f & F_CT)) {
    ctLights[i].setOnOff(st[i].on);
    ctLights[i].setBrightness(st[i].bri);
    ctLights[i].setColorTemperature(st[i].mired);
  } else if (f & F_LIGHT) {
    onoffLights[i].setOnOff(st[i].on);
  }
  if (f & F_FAN) {
    syncMatterFan(i);
  }
  fromWeb = false;
}
#else
static void syncMatter(int) {}
#endif

// ---------------- web API ----------------
static String jsonStr(const char *s) {
  String o = "\"";
  for (; *s; s++)
    if (*s == '"' || *s == '\\') o += '\\', o += *s;
    else if ((uint8_t)*s >= 0x20) o += *s;
  return o + "\"";
}

static String ipString() {
  esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip;
  if (n && esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr) {
    char b[16];
    snprintf(b, sizeof(b), IPSTR, IP2STR(&ip.ip));
    return b;
  }
  return "not connected";
}

static String macStr(const uint8_t m[6]) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return b;
}

static void handleState() {
  String j;
  j.reserve(4500);
  j += "{\"matter\":{";
#if USE_MATTER
  j += "\"enabled\":true,\"commissioned\":";
  j += Matter.isDeviceCommissioned() ? "true" : "false";
  j += ",\"code\":" + jsonStr(Matter.getManualPairingCode().c_str());
  j += ",\"qr\":" + jsonStr(Matter.getOnboardingQRCodeUrl().c_str());
#else
  j += "\"enabled\":false";
#endif
  j += "},\"ip\":" + jsonStr(ipString().c_str());
  j += ",\"selftest\":" + String(selfTestOk ? "true" : "false");
  j += ",\"rebootNeeded\":" + String(rebootNeeded ? "true" : "false");
  j += ",\"learning\":" + String(isLearning() ? "true" : "false");
  j += ",\"mcold\":" + String(MIREDS_COLD) + ",\"mwarm\":" + String(MIREDS_WARM);
  j += ",\"variants\":[";
  for (int v = 0; v < NUM_VARIANTS; v++) j += (v ? "," : "") + jsonStr(VARIANTS[v].label);
  j += "],\"models\":[";
  for (int m = 0; m < tuiss::NUM_MODELS; m++) j += (m ? "," : "") + jsonStr(tuiss::MODELS[m]);
  j += "],\"devices\":[";
  for (int i = 0; i < numDevs; i++) {
    const Device &d = devs[i];
    const DevState &s = st[i];
    bool busy = tuiss::busyDevice() == i || tuiss::pending(i);
    char b[560];
    snprintf(b, sizeof(b),
             "%s{\"i\":%d,\"kind\":%u,\"name\":%s,\"id\":%lu,\"index\":%u,\"variant\":%u,\"speeds\":%u,"
             "\"light\":%s,\"ct\":%s,\"fan\":%s,\"model\":%u,\"mac\":\"%s\","
             "\"st\":{\"on\":%s,\"bri\":%u,\"mired\":%u,\"fan\":%u,\"dir\":%u,\"pos\":%d,\"target\":%d,\"bat\":%u,"
             "\"busy\":%s,\"msg\":%s}}",
             i ? "," : "", i, d.kind, jsonStr(d.name).c_str(), (unsigned long)d.id, d.index, d.variant, d.speeds,
             (d.flags & F_LIGHT) ? "true" : "false", (d.flags & F_CT) ? "true" : "false", (d.flags & F_FAN) ? "true" : "false",
             d.model, macStr(d.mac).c_str(), s.on ? "true" : "false", s.bri, s.mired, s.fan, s.dir, s.pos, s.target, s.bat,
             busy ? "true" : "false", jsonStr(s.msg).c_str());
    j += b;
  }
  j += "],\"learned\":[";
  portENTER_CRITICAL(&mux);
  Learned L[8];
  uint8_t nl = numLearned;
  memcpy(L, learned, sizeof(L));
  portEXIT_CRITICAL(&mux);
  for (int i = 0; i < nl; i++) {
    char b[120];
    snprintf(b, sizeof(b), "%s{\"id\":%lu,\"index\":%u,\"variant\":%u,\"cmd\":%u,\"rssi\":%d}", i ? "," : "",
             (unsigned long)L[i].d.id, L[i].d.index, L[i].d.variant, L[i].d.cmd, L[i].rssi);
    j += b;
  }
  j += "],\"blinds\":[";
  tuiss::Seen S[8];
  uint8_t ns = tuiss::copySeen(S);
  for (int i = 0; i < ns; i++) {
    String mf;
    for (int k = 0; k < S[i].mfrLen; k++) {
      char h[3];
      snprintf(h, sizeof(h), "%02X", S[i].mfr[k]);
      mf += h;
    }
    char b[200];
    snprintf(b, sizeof(b), "%s{\"name\":%s,\"mfr\":\"%s\",\"rssi\":%d,\"age\":%lu}", i ? "," : "", jsonStr(S[i].name).c_str(),
             mf.c_str(), S[i].rssi, (unsigned long)((millis() - S[i].lastMs) / 1000));
    j += b;
  }
  j += "]}";
  server.send(200, "application/json", j);
}

static int argInt(const char *n, int def = 0) {
  return server.hasArg(n) && server.arg(n).length() ? server.arg(n).toInt() : def;
}

static void handleCmd() {
  int i = argInt("d", -1);
  if (i < 0 || i >= numDevs) return server.send(400, "text/plain", "Unknown device");
  String c = server.arg("c");
  int v = argInt("v");
  DevState &s = st[i];
  if (isBlind(i)) {
    if (c == "open") blindRequest(i, tuiss::W_SET, 100);
    else if (c == "close") blindRequest(i, tuiss::W_SET, 0);
    else if (c == "pos") blindRequest(i, tuiss::W_SET, constrain(v, 0, 100));
    else if (c == "stop") blindRequest(i, tuiss::W_STOP);
    else if (c == "refresh") blindRequest(i, tuiss::W_REFRESH);
    else return server.send(400, "text/plain", "Unknown blind command");
    syncMatter(i);
    return server.send(200, "text/plain", bleReady() ? "ok" : "queued (radio not ready)");
  }
  if (c == "light") {
    s.on = v;
    qLight(i, s.on);
  } else if (c == "toggle") {
    s.on = !s.on;
    enqueue({(uint8_t)i, CMD_LIGHT_TOGGLE, 0, 0, 0});
  } else if (c == "bri") {
    s.bri = constrain(v, 1, 255);
    s.on = true;
    qCt(i);
  } else if (c == "ct") {
    s.mired = constrain(v, MIREDS_COLD, MIREDS_WARM);
    s.on = true;
    qCt(i);
  } else if (c == "fan") {
    s.fan = constrain(v, 0, devs[i].speeds);
    qFan(i, s.fan);
  } else if (c == "dir") {
    s.dir = v;
    enqueue({(uint8_t)i, CMD_DIRECTION, (uint8_t)(v ? 1 : 0), 0, 0});
  } else if (c == "pair") {
    enqueue({(uint8_t)i, CMD_PAIR, 0, 0, 0});
  } else if (c == "unpair") {
    enqueue({(uint8_t)i, CMD_UNPAIR, 0, 0, 0});
  } else {
    return server.send(400, "text/plain", "Unknown command");
  }
  syncMatter(i);
  server.send(200, "text/plain", bleReady() ? "ok" : "queued (radio not ready)");
}

// Returns an error message, or nullptr if the form is valid.
static const char *readDeviceForm(Device &d) {
  d.kind = argInt("kind") == KIND_BLIND ? KIND_BLIND : KIND_FAN;
  String name = server.arg("name");
  int n = 0;
  for (size_t k = 0; k < name.length() && n < 22; k++) {
    char ch = name[k];
    if (isalnum((unsigned char)ch) || ch == ' ' || ch == '-' || ch == '_' || ch == '\'') d.name[n++] = ch;
  }
  d.name[n] = 0;
  if (!n) strcpy(d.name, d.kind == KIND_BLIND ? "Blind" : "Fan");

  if (d.kind == KIND_BLIND) {
    if (!tuiss::parseMac(server.arg("mac").c_str(), d.mac)) return "Blind ID must look like AA:BB:CC:DD:EE:FF (from the blind's label)";
    d.model = constrain(argInt("model", 5), 0, tuiss::NUM_MODELS - 1);
    d.flags = 0;
    return nullptr;
  }
  String id = server.arg("id");
  id.trim();
  if (id.length()) {
    char *end;
    d.id = strtoul(id.c_str(), &end, 16);
    if (*end || !d.id) return "Remote ID must be hex, e.g. 034A5A9D";
  } else if (!d.id) {
    do d.id = esp_random();
    while (!d.id);
  }
  d.index = constrain(argInt("index"), 0, 255);
  d.variant = constrain(argInt("variant", VAR_FL_V3), 0, NUM_VARIANTS - 1);
  d.speeds = argInt("speeds", 6) == 3 ? 3 : 6;
  d.flags = (argInt("light") ? F_LIGHT : 0) | (argInt("ct") ? F_CT : 0) | (argInt("fan") ? F_FAN : 0);
  return nullptr;
}

static void markChanged() {
#if USE_MATTER
  rebootNeeded = true;
#endif
  saveDevices();
}

static void handleAdd() {
  if (numDevs >= MAX_DEVICES) return server.send(400, "text/plain", "Device limit reached");
  Device d = {};
  if (const char *err = readDeviceForm(d)) return server.send(400, "text/plain", err);
  devs[numDevs] = d;
  st[numDevs] = defaultState();
  numDevs++;
  markChanged();
  if (d.kind == KIND_BLIND) blindRequest(numDevs - 1, tuiss::W_REFRESH);
  server.send(200, "text/plain", "ok");
}

static void handleEdit() {
  int i = argInt("d", -1);
  if (i < 0 || i >= numDevs) return server.send(400, "text/plain", "Unknown device");
  Device d = devs[i];
  if (const char *err = readDeviceForm(d)) return server.send(400, "text/plain", err);
  bool matterChange = d.flags != devs[i].flags || d.kind != devs[i].kind;
  bool macChange = memcmp(d.mac, devs[i].mac, 6) != 0;
  devs[i] = d;
  if (matterChange) markChanged();
  else saveDevices();
  if (d.kind == KIND_BLIND && macChange) blindRequest(i, tuiss::W_REFRESH);
  server.send(200, "text/plain", "ok");
}

static void handleDel() {
  int i = argInt("d", -1);
  if (i < 0 || i >= numDevs) return server.send(400, "text/plain", "Unknown device");
  portENTER_CRITICAL(&mux);
  numJobs = 0;  // drop queued commands (device indexes shift)
  portEXIT_CRITICAL(&mux);
  tuiss::clearAll();
  for (int k = i; k < numDevs - 1; k++) devs[k] = devs[k + 1], st[k] = st[k + 1];
  numDevs--;
  markChanged();
  server.send(200, "text/plain", "ok");
}

static bool restartPending = false;

static void setupWeb() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/cmd", HTTP_POST, handleCmd);
  server.on("/api/dev/add", HTTP_POST, handleAdd);
  server.on("/api/dev/edit", HTTP_POST, handleEdit);
  server.on("/api/dev/del", HTTP_POST, handleDel);
  server.on("/api/learn", HTTP_POST, [] {
    if (startLearn()) server.send(200, "text/plain", "ok");
    else server.send(503, "text/plain", "Radio not ready (add FanBridge to Google Home first)");
  });
  server.on("/api/reboot", HTTP_POST, [] {
    server.send(200, "text/plain", "ok");
    restartPending = true;
  });
  server.on("/api/matter/reset", HTTP_POST, [] {
#if USE_MATTER
    server.send(200, "text/plain", "ok");
    Matter.decommission();
#else
    server.send(400, "text/plain", "Matter is disabled");
#endif
  });
  server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

// ---------------- serial + button ----------------
static void handleSerial() {
  static String line;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch != '\n' && ch != '\r') {
      line += ch;
      continue;
    }
    line.trim();
    if (!line.length()) continue;
    char ssid[33] = {0}, pass[65] = {0};
    if (line == "ip") Serial.printf("IP: %s\r\n", ipString().c_str());
    else if (sscanf(line.c_str(), "wifi %32s %64s", ssid, pass) >= 1) {
#if USE_MATTER
      Serial.println("In Matter mode Wi-Fi is set when you add FanBridge in Google Home.");
#else
      WiFi.begin(ssid, pass);
      Serial.printf("Connecting to %s...\r\n", ssid);
#endif
    } else if (line == "list") {
      for (int i = 0; i < numDevs; i++) {
        if (isBlind(i))
          Serial.printf("%d: %s blind %s id=%s pos=%d\r\n", i, devs[i].name, tuiss::MODELS[devs[i].model], macStr(devs[i].mac).c_str(),
                        st[i].pos);
        else
          Serial.printf("%d: %s fan id=%08lX idx=%u %s\r\n", i, devs[i].name, (unsigned long)devs[i].id, devs[i].index,
                        VARIANTS[devs[i].variant].key);
      }
    } else {
      Serial.println("Commands: ip | list | wifi <ssid> <password> (test mode only). Everything else is on the web page.");
    }
    line = "";
  }
}

static void handleButton() {  // BOOT: short press = toggle the first fan's light, hold 5 s = remove from Google Home
  static uint32_t pressedAt = 0;
  static bool down = false, longDone = false;
  bool now = digitalRead(BOOT_PIN) == LOW;
  if (now && !down) down = true, longDone = false, pressedAt = millis();
  else if (!now && down) {
    down = false;
    if (!longDone && millis() - pressedAt > 50) {
      for (int i = 0; i < numDevs; i++)
        if (devs[i].kind == KIND_FAN && (devs[i].flags & F_LIGHT)) {
          st[i].on = !st[i].on;
          enqueue({(uint8_t)i, CMD_LIGHT_TOGGLE, 0, 0, 0});
          syncMatter(i);
          break;
        }
    }
  } else if (now && !longDone && millis() - pressedAt > 5000) {
    longDone = true;
#if USE_MATTER
    Serial.println("Removing from Google Home (decommission)...");
    Matter.decommission();
#endif
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  pinMode(BOOT_PIN, INPUT_PULLUP);
  prefs.begin("fanbridge", false);
  loadDevices();
  selfTestOk = selfTest();
  if (!ESP.getPsramSize())
    Serial.println("WARNING: PSRAM is off. Arduino IDE > Tools > PSRAM > OPI PSRAM, then upload again (needed for blinds).");
  Serial.printf("\r\nFanBridge - encoder self-test: %s, %u device(s)\r\n", selfTestOk ? "PASS" : "FAIL", numDevs);
#if USE_MATTER
  setupMatter();
#else
  BLEDevice::init("");
  WiFi.mode(WIFI_STA);
  WiFi.begin();  // uses saved credentials, if any
#endif
  tuiss::begin(gapEvent, bleReady, onBlindResult);
  setupWeb();
}

void loop() {
  static uint32_t lastInfo = 0, lastBlindRefresh = 0;
  static String lastIp;
  static bool wasReady = false;
  if (millis() - lastInfo > 10000) {
    lastInfo = millis();
    String ip = ipString();
    if (ip != lastIp) {
      lastIp = ip;
      Serial.printf("Web page: http://%s/\r\n", ip.c_str());
    }
    Serial.printf("Free memory: %u (lowest %u), PSRAM free %u, blind task stack left %u%s\r\n", ESP.getFreeHeap(),
                  ESP.getMinFreeHeap(), ESP.getFreePsram(), tuiss::workerStackFree(),
                  tuiss::busyDevice() >= 0 ? ", blind busy" : "");
#if USE_MATTER
    if (!Matter.isDeviceCommissioned()) {
      Serial.println("Not in Google Home yet: Google Home app > + > Device > Matter-enabled device.");
      Serial.printf("Manual pairing code: %s\r\nQR code: %s\r\n", Matter.getManualPairingCode().c_str(),
                    Matter.getOnboardingQRCodeUrl().c_str());
    }
#endif
  }

  // read every blind's position once the radio is free, then periodically
  bool ready = bleReady();
  if (ready && !wasReady) {
    refreshAllBlinds();
    lastBlindRefresh = millis();
  }
  wasReady = ready;
  if (BLIND_REFRESH_MIN && ready && millis() - lastBlindRefresh > BLIND_REFRESH_MIN * 60000UL) {
    lastBlindRefresh = millis();
    refreshAllBlinds();
  }
  if (isLearning()) scanFor(learnEnd - millis());  // keep listening if a blind connection interrupted the scan

  server.handleClient();
  handleSerial();
  handleButton();
  serviceRadio();
#if USE_MATTER
  for (int i = 0; i < bootCount; i++) {
    if (snapPending[i]) {  // snap Google's fan slider to the step actually sent
      snapPending[i] = false;
      if (!rebootNeeded && (bootFlags[i] & F_FAN)) syncMatterFan(i);
    }
    if (blindSyncPending[i]) {  // report a blind's new position to Google
      blindSyncPending[i] = false;
      if (!rebootNeeded && bootKind[i] == KIND_BLIND) syncMatterBlind(i);
    }
  }
#endif
  if (restartPending && !burstEnd && !tuiss::radioBusy()) {
    delay(200);
    ESP.restart();
  }
  delay(2);
}

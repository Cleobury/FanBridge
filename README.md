# FanBridge

**Control Philips Bliss ceiling fans (and other "FanLamp Pro" fans) and Tuiss smart blinds from Google Home and a web page, using a £10 ESP32 board.**

The Philips Bliss fan ceiling light (for example [8720169279087](https://www.lighting.philips.co.uk/consumer/p/fan-ceiling-lights-bliss-fan-ceiling-light-35-28-w/8720169279087)) ships with a 2.4 GHz remote. It isn't a proprietary radio or a Bluetooth connection: the remote **broadcasts Bluetooth LE advertisements** in the "FanLamp Pro" format used by many fan lights. FanBridge runs on a Seeed XIAO ESP32-S3, broadcasts the same packets, and makes your fans appear in **Google Home** (via Matter) and on a **web page** on your Wi-Fi.

- Light on/off, brightness, warm/cool white
- Fan off + speeds 1–6 (or 1–3), forward/reverse
- **Tuiss SmartView blinds**: open / close / stop / set position, position and battery read-back
- Up to 8 devices (fans and blinds) from one board
- Clone your existing remote (no pairing) **or** pair FanBridge as a new remote
- Your original remote and the FanLamp Pro app keep working

> Not affiliated with Philips or Signify. Use at your own risk.

---

## What's been tested

| Feature | Status |
|---|---|
| Decoding the Bliss remote; encoder reproduces real captures byte-for-byte | ✅ verified |
| Light and fan speeds from the web page (cloned remote) | ✅ verified on a Bliss fan |
| Fan and light from Google Home (Nest hub, Matter) | ✅ verified |
| Multiple devices | ✅ verified |
| Brightness / colour temperature | implemented; depends on your fan model |
| Pairing a new random ID, "Learn" button, LampSmart Pro variants | implemented from the protocol; less tested |
| Tuiss blinds (web page + Google Home) | **new, not yet tested on hardware** – protocol from TuissESP (verified there on a TS5001) |

---

## What you need

| Item | Notes |
|---|---|
| Philips Bliss fan with its remote | Other FanLamp Pro / LampSmart Pro fans should also work |
| **Seeed Studio XIAO ESP32-S3** | **Attach the little antenna that comes with it** – without it Wi-Fi won't connect |
| USB-C **data** cable | Charge-only cables won't show a COM port |
| Google Nest speaker, display or hub | Acts as the Matter controller. Not needed for web-only mode |
| 2.4 GHz Wi-Fi | The ESP32-S3 has no 5 GHz radio. WPA2 or WPA2/WPA3 mixed |
| A PC with [Arduino IDE 2.x](https://www.arduino.cc/en/software) | |
| *(Optional)* Tuiss SmartView blinds | TS3000, TS5200, TS5300, TS2600, TS2900, TS5001, TS5101. You need each blind's ID from its label |
| *(Optional)* Android phone with **nRF Connect** | To check your fan uses this protocol, and for debugging |

### Check your fan is compatible (2 minutes, optional)
1. Install **nRF Connect** (Nordic Semiconductor) on an Android phone and start a scan.
2. Press a button on the fan remote.
3. Look for a device with **Service Data UUID `0x08F0`** (data starting `0x10 00…`) and/or **Manufacturer data, Company `0x5556`** (data starting `0x18 87 52…`).

If you see those, FanBridge will work. You can paste the data into the decoder to see what it says:
```bash
pip install cryptography
python tools/fan_decode.py 0x1000A26CC1067B5C9750C056C2D745272E6822B0C554D85A
```

---

## Setup

### 1. Install ESP32 support in the Arduino IDE
1. **File → Preferences → Additional boards manager URLs**, add:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
2. **Tools → Board → Boards Manager**, search **esp32**, install **"esp32 by Espressif Systems" version 3.3.x** (tested with **3.3.11**).
   **Do not use 4.0.0-alpha** – it removes a library FanBridge needs (`mbedtls/aes.h: No such file or directory`). If the IDE offers to update the ESP32 package, decline.

### 2. Get the code
```bash
git clone https://github.com/Cleobury/FanBridge.git
```
Or use **Code → Download ZIP** on GitHub and extract it. Open **`FanBridge/FanBridge.ino`** in the Arduino IDE (the other `.h` files open as tabs).

### 3. Board settings and upload
Plug in the XIAO, then set:

| Setting (Tools menu) | Value |
|---|---|
| Board | **esp32 → XIAO_ESP32S3** (not the Silicon Labs "XIAO MG24", not a Seeed SAMD board) |
| Port | the new COM port |
| USB CDC On Boot | Enabled |
| Partition Scheme | **Default with spiffs (3MB APP/1.5MB SPIFFS)** |
| PSRAM | **OPI PSRAM** (needed for blinds: without it the board runs out of memory and Wi-Fi drops) |
| Everything else | defaults |

Click **Upload**. If no COM port appears, hold the tiny **B** (boot) button, plug the cable in, release, then upload; press **R** (reset) afterwards.

Open **Tools → Serial Monitor** at **115200** baud. You should see:
```
FanBridge - encoder self-test: PASS, 1 device(s)
Not in Google Home yet: Google Home app > + > Device > Matter-enabled device.
Manual pairing code: 3497-011-2332
QR code: https://project-chip.github.io/connectedhomeip/qrcode.html?data=MT:...
```

### 4. Add FanBridge to Google Home
1. Phone on your **main** home Wi-Fi (not guest, no VPN), Bluetooth on, near the XIAO.
2. Google Home app → **+** → **Device** → **New device** → **Matter-enabled device**.
3. Scan the QR code (open the `QR code:` link from the Serial Monitor on your PC) or type the manual pairing code.
4. Google warns it's an **uncertified device** – continue. Google sends FanBridge your Wi-Fi details during this step.
5. The devices appear as **TEST_DEVICE**. That's normal for uncertified Matter devices: tap each one → ⚙ → **Name** and rename them (e.g. "Bedroom fan" and "Bedroom fan light").

### 5. Open the web page
The Serial Monitor prints `Web page: http://192.168.x.x/` once FanBridge is on Wi-Fi (type `ip` + Enter to print it again). Open that address on any phone or PC on the same Wi-Fi. Tip: give FanBridge a **DHCP reservation** in your router so the address never changes, then use **Add to Home screen** in your phone's browser.

### 6. Connect FanBridge to your fan
FanBridge starts with one device called **"Ceiling fan"**. Give it an identity the fan accepts – pick one:

**Option A – clone your remote (recommended, no pairing)**
1. Fan powered on. On the web page press **Listen 20 s** (under *Learn from a remote or app*).
2. Press any button on your fan's remote (or in the FanLamp Pro app, if you use it).
3. The remote's ID appears. Press **Apply to Ceiling fan**.

**Option B – pair FanBridge as a new remote**
1. Turn the fan **off at the wall**, wait 5 s, turn it **back on**.
2. Within ~5 seconds, open *Pairing & settings* on the device card and press **Pair**. The light usually blinks to confirm.

Now test the light and fan buttons on the web page, then from Google Home ("Hey Google, turn on the bedroom fan light", "set the bedroom fan to 50%").

### Adding Tuiss blinds
Blinds need no pairing: FanBridge connects to each blind over Bluetooth whenever it sends a command (that's how the Tuiss app works too), so every command takes a few seconds.

1. Find the **blind ID** – a MAC-style code such as `AA:BB:CC:DD:EE:FF` on the label on the motor or bracket (also shown in the Tuiss app). It is *not* always the address nRF Connect shows.
2. On the web page, under **Add a device**, set **Type → Tuiss smart blind**, enter a name, the blind ID and the model (e.g. TS5001), and press **Add device**. (Tip: **Listen 20 s** lists Tuiss blinds in range with their advert data – your label's ID appears inside it, and **Add blind** fills in the model.)
3. FanBridge immediately reads the blind's position and battery. The card shows *Working…* and then the result, e.g. `OK` or `Blind not found - out of range?`.
4. Press **Reboot now** so Google Home gets the new blind. It appears as a **window covering**: "Hey Google, open the bedroom blind", "set the bedroom blind to 30%", "close all the blinds". If it doesn't appear, remove FanBridge from Google Home and add it again.

Blind positions are read back at boot and every 30 minutes (`BLIND_REFRESH_MIN`), so changes made with the Tuiss app or remote show up eventually; press **Refresh** on the card to read it straight away.

### Adding more fans
1. Use **Learn → Add as new**, or fill in **Add a device** (blank ID = new random ID, then **Pair**).
2. Press **Reboot now** when the banner asks – new devices only reach Google Home after a restart.
3. If a new device doesn't show up in Google Home, remove FanBridge from Google Home (web page → *Remove from Google Home*) and add it again (step 4).

---

## Using it

### Web page
Each device card has light on/off/toggle, brightness and warmth sliders, fan speed buttons and direction. Under **Pairing & settings**: Pair, Unpair, Edit (name, packet type, ID, speeds, features) and Delete.

### Google Home
Each device is a **light** (on/off, brightness, colour temperature) and a **fan**. Google always shows Matter fans as a percentage; FanBridge maps it to the nearest real speed and snaps the slider to it:

| Google % | 0 | 1–24 | 25–41 | 42–58 | 59–74 | 75–91 | 92–100 |
|---|---|---|---|---|---|---|---|
| Speed (6-speed fan) | off | 1 | 2 | 3 | 4 | 5 | 6 |

Low / Medium / High → speeds 2 / 3 / 6. "Turn on the fan" restores the last speed.

### Blinds
Each blind card has an **Open %** slider (Tuiss and the web page count percent *open*; Google Home shows the same position), **Open / Stop / Close**, **Refresh** (reads position and battery) and a battery badge. Google Home: open, close, set a percentage, stop.

### BOOT button on the XIAO
- Short press: toggle the first device's light
- Hold 5 s: remove FanBridge from Google Home (so it can be added again)

### Settings at the top of `FanBridge.ino`
| Setting | Default | Meaning |
|---|---|---|
| `USE_MATTER` | `1` | `0` = web page only (set Wi-Fi in the Serial Monitor with `wifi <ssid> <password>`) |
| `MAX_DEVICES` | `8` | Maximum number of fans |
| `MIREDS_COLD` / `MIREDS_WARM` | `153` / `370` | Colour-temperature range (6500 K / 2700 K) |
| `BURST_MS` | `800` | How long each command is broadcast |
| `LEARN_MS` | `20000` | How long Learn listens |
| `BLIND_REFRESH_MIN` | `30` | Re-read blind positions this often (minutes, 0 = off) |

### Limitations
- **One-way radio.** The fan never reports its state. If you use the physical remote, the web page and Google Home won't know.
- Pairing, Learn and sending only work once FanBridge has been added to Google Home (Matter needs Bluetooth during setup). In `USE_MATTER 0` mode they work as soon as it boots.
- Changing the device list needs a reboot before Google Home sees it.
- Blinds take a few seconds per command (scan, connect, send, disconnect). While a blind connection is open, fan commands wait.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| `Preferences.h: No such file or directory` | Wrong board selected – choose **esp32 → XIAO_ESP32S3** |
| `mbedtls/aes.h: No such file or directory` | ESP32 core 4.0 alpha installed – downgrade to **3.3.x** in Boards Manager |
| Compile hangs for > 15 min | Close other Arduino IDE windows/compiles and try again; a full Matter build takes several minutes the first time |
| Google Home connects over Bluetooth, then fails to join Wi-Fi | Attach the **antenna**; use a **2.4 GHz** network (not 5 GHz-only, not WPA3-only, not hidden, not guest); disable AP/client isolation; then hold BOOT 5 s, press R and try again. Set *Tools → Core Debug Level → Info* to see Wi-Fi errors |
| `Data received on an unknown session … Dropping it!` | Normal for a minute after FanBridge reboots – Google is still using the old session. Wait 1–2 min; if it never stops, restart your Nest |
| `Google Home -> … (IGNORED: device list changed…)` | Press **Reboot now** on the web page |
| No `Google Home ->` line when you use Google Home | Commands aren't reaching FanBridge – see the two rows above; check Google shows the device online |
| `TX … sent` but the fan does nothing | Move the XIAO closer; check the device's ID/packet type (use Learn); try **Pair**; confirm with nRF Connect that FanBridge's packets appear |
| Blind card shows `Blind not found - out of range?` | Check the blind ID against the label; move the XIAO closer; press **Listen 20 s** to see whether the blind appears at all. Some blinds sleep – moving it with its own remote wakes it |
| Blind shows `Connect failed` / `Subscribe failed` | Usually range; try again. Make sure the Tuiss app isn't connected to the blind at the same moment |
| `TX … FAILED (rc=…)` | Bluetooth not ready – reboot; report the `rc` number |
| `Guru Meditation Error` / reboot loop | Please open an issue with the full backtrace and the `ELF file SHA256` line |
| `E (…) wifi:sta is connecting, return error` at boot | Harmless |

---

## How it works

Every remote button press is broadcast for a moment as a 31-byte BLE advertisement. The payload is whitened with a seed-dependent table, signed with a 16-bit AES-derived value, and protected by a CRC-16. Inside is a remote ID, a group index, a rolling counter and a command byte (`0x10` light on, `0x31` fan speed, …). The fan obeys any sender whose ID it has paired with. FanBridge re-creates these packets exactly. See **[docs/PROTOCOL.md](docs/PROTOCOL.md)** for the full format.

Tuiss blinds are different: each command is a short BLE **connection** – scan for the blind's advert (matched by the ID on its label), connect, subscribe to notifications, send a keepalive and a timestamp, send the command, read the reply, disconnect. See **[docs/TUISS.md](docs/TUISS.md)**.

```
Google Home ──Matter/Wi-Fi──┐                               ┌──BLE adverts─────> fans
                             ├──> XIAO ESP32-S3 (FanBridge) ─┤
Phone browser ──HTTP/Wi-Fi──┘                               └──BLE connections─> Tuiss blinds
```

## Repository layout

```
FanBridge/            Arduino sketch (open FanBridge.ino)
  FanBridge.ino       devices, Matter (Google Home), web API, radio queue
  fanlamp.h           packet encoder/decoder + self-test against real captures
  tuiss.h             Tuiss blind driver (BLE GATT client on the shared NimBLE stack)
  web_ui.h            the web page
  types.h             shared structs
tools/
  fan_decode.py       decode captures pasted from nRF Connect
  fan_encode.py       reference encoder; checks itself against real/reference packets
docs/PROTOCOL.md      the fan over-the-air format
docs/TUISS.md         the Tuiss blind protocol
```

## Credits and licence

The packet format (whitening table, signature and command set) was worked out by the [ha-ble-adv](https://github.com/NicoIIT/ha-ble-adv) project by @NicoIIT (MIT licence); FanBridge is an independent implementation verified against captures from a Philips Bliss remote. The Tuiss blind protocol comes from [TuissESP](https://github.com/ba1i95/TuissESP) by Callum Wallace (MIT licence). See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Philips is a trademark of Koninklijke Philips N.V.; this project is not affiliated with, endorsed by, or supported by Philips or Signify.

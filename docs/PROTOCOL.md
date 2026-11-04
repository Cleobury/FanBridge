# The FanLamp BLE advertising protocol (as used by Philips Bliss fans)

The remote never connects to the fan. Each button press is **broadcast as BLE advertisements** for a moment, and the fan acts on any packet from an ID it has been paired with. There is no acknowledgement and no state report.

A Bliss remote sends every press in **two formats at once**, from two random MAC addresses. That's why nRF Connect shows two devices:

| Format | Where | Used by FanBridge |
|---|---|---|
| **v2/v3** | Service Data, 16-bit UUID `0x08F0` | ✅ sent and decoded |
| **v1** | Manufacturer Data, company ID `0x5556` | decoded by `tools/fan_decode.py` only |

The FanLamp Pro phone app sends the same v2/v3 structure with a different prefix and AD type (see *Variants*).

---

## v2/v3 packet

### Advertising data (31 bytes, legacy advertising)

```
02 01 FF            Flags AD (FF = 0x02 remote, 0x19 app)
1B TT F0 08         AD length 27, AD type TT (0x16 Service Data = remote, 0x03 UUID list = app), then F0 08
<24 bytes>          payload, below
```

### Payload (24 bytes on air)

```
offset  0  1 | 2 ............................ 19 | 20 21 | 22 23
        P0 P1|  whitened bytes d[2..19]          | seed  | CRC16
```

- **seed**: 16-bit random value, little-endian, picked fresh for every command.
- **CRC16**: CRC-16/CCITT (poly `0x1021`, no reflection, as Python `binascii.crc_hqx`) over bytes 0–21, initial value `seed ^ 0xFFFF`, stored little-endian.
- **Whitening**: `out[i] = XBOX[((seed8 + i + 9) & 0x1F) + salt] ^ seed8 ^ in[i]`, where `seed8 = seed & 0xFF`, `salt = (P1 & 3) << 5`, `i` = 0…17 over `d[2..19]`, and `XBOX` is the 128-byte table in [`fanlamp.h`](../FanBridge/fanlamp.h). Whitening is its own inverse.

### Decoded bytes `d[0..21]`

| Byte(s) | Meaning |
|---|---|
| 0–2 | Prefix / variant, e.g. `10 00 56` (Bliss remote), `20 80 00` (FanLamp Pro app v3) |
| 3 | Rolling counter (tx). Goes up by one per press |
| 4–5 | Device type, little-endian: `0x0400` FanLamp, `0x0100` LampSmart |
| 6–9 | **Remote ID**, little-endian (uint32) |
| 10 | Group index |
| 11 | **Command** |
| 12 | 0 |
| 13 | param |
| 14–16 | arg0, arg1, arg2 |
| 17–18 | Signature (v3) or `00 00` (v2), little-endian |
| 19 | 0 |
| 20–21 | seed (same as on air) |

**Signature (v3 variants):** AES-128-ECB encrypt `d[1..16]` with key
`[seed_lo, seed_hi, tx, 0D BF E6 42 68 41 99 2D 0F B0 54 BB 16]`. The signature is the first two output bytes, little-endian (`0` becomes `0xFFFF`).

### Variants

| FanBridge key | Prefix | Signed | Device type | AD type | Flags | Sender |
|---|---|---|---|---|---|---|
| `remote` | `10 00 56` | yes | `0x0400` | `0x16` | `0x02` | Philips Bliss physical remote |
| `fl_v3` | `20 80 00` | yes | `0x0400` | `0x03` | `0x19` | FanLamp Pro app (v3) |
| `fl_v2` | `10 80 00` | no | `0x0400` | `0x03` | `0x19` | FanLamp Pro app (v2) |
| `ls_v3` | `30 80 00` | yes | `0x0100` | `0x03` | `0x19` | LampSmart Pro app (v3) |
| `ls_v2` | `10 80 00` | no | `0x0100` | `0x03` | `0x19` | LampSmart Pro app (v2) |

### Commands

| Cmd | Meaning | Args |
|---|---|---|
| `0x09` | Light toggle (what the Bliss remote's light button sends) | – |
| `0x10` / `0x11` | Light on / off | – |
| `0x12` / `0x13` | Second light on / off | – |
| `0x15` | Fan direction | arg0: 0 forward, 1 reverse |
| `0x21` | Light colour temperature + brightness | arg0 = 0, arg1 = cold 0–255, arg2 = warm 0–255 |
| `0x23` | Night mode | – |
| `0x28` | **Pair** (send within ~5 s of powering the fan on) | – |
| `0x31` | Fan speed | arg0 = `0x20` (6-speed) or `0x00` (3-speed), arg1 = speed, 0 = off |
| `0x45` | Unpair | – |

### Worked example: the Bliss remote's light button

Captured Service Data (after `F0 08`):
```
10 00 A2 6C C1 06 7B 5C 97 50 C0 56 C2 D7 45 27 2E 68 22 B0 C5 54 D8 5A
```
seed = `0x54C5`, CRC valid. Un-whitened:
```
10 00 56 | BC | 00 04 | 9D 5A 4A 03 | 00 | 09 | 00 | 00 | 00 00 00 | 8A 55 | 00 | C5 54
prefix    tx=188  dev    id=0x034A5A9D idx  cmd=toggle            sign
```
The next two presses had tx 189 and 187. Everything else in the payload looks different only because the seed changes.

---

## v1 packet (Manufacturer Data, company `0x5556`)

```
56 55 18 87 52 | B6 5F 2B 5E 00 FC 31 51 | 16 bytes
company+header   whitened fixed prefix   payload
```
Payload: LFSR-whitened (7-bit, seed `0x0C`), then each byte bit-reversed. The last two bytes are a fixed `0x9372` marker. The 14 decoded bytes are:
`cmd, group(2, LE), arg0, arg1, arg2, tx, param, r1, r2 (= seed8 ^ 1), seed(2, BE), crc16(2, BE)`,
where the CRC is CRC-16/CCITT over the first 12 bytes with initial value `seed ^ 0xFFFF`. Commands are the same as v2/v3. See `decode_v1()` in [`tools/fan_decode.py`](../tools/fan_decode.py).

---

## Checking your own captures

```bash
pip install -r tools/requirements.txt
python tools/fan_decode.py <service-data-hex> [<manufacturer-data-hex> ...]
python tools/fan_encode.py      # self-check: re-creates real and reference packets
```

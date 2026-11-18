# Tuiss SmartView blind protocol

Source: [TuissESP](https://github.com/ba1i95/TuissESP) by Callum Wallace (MIT), itself building on Tuiss2HA. FanBridge's implementation is in [`FanBridge/tuiss.h`](../FanBridge/tuiss.h).

Supported models: TS3000, TS5200, TS5300, TS2600, TS2900, TS5001, TS5101.

## Finding the blind

The blind advertises with its **model name** (e.g. `TS5001`, in the scan response) and **manufacturer data containing the blind ID** – the MAC-style ID printed on the blind's label. That ID is not necessarily the live BLE address, so FanBridge scans actively and matches the ID inside the manufacturer data (either byte order), then connects to whatever address that advert came from.

## Session (one per command)

1. Scan until an advert contains the blind ID (FanBridge gives up after 30 s).
2. Connect (6 s timeout, one retry).
3. Discover characteristics:
   - write: `00010405-0405-0607-0809-0a0b0c0d1910`
   - notify: `00010304-0405-0607-0809-0a0b0c0d1910`
4. Enable notifications (write `01 00` to the notify characteristic's CCCD).
5. Write the **keepalive**: `FF 03 03 03 03 78 78 78 78 78 78`
6. Write a **timestamp**: `FF 78 EA 41 02 00 YY MM DD hh mm ss` (YY = year − 2000).
7. Write the **command** (below), with response.
8. Wait up to 3.5 s for notifications, then disconnect.

## Commands

All commands start with `FF 78 EA 41`.

| Command | Bytes after the header |
|---|---|
| Set position | `BF 03 LL HH` – percent **open** × 10 as uint16 little-endian (0 = closed, 1000 = fully open) |
| Open / Close | set position 100 % / 0 % |
| Stop | `5F 03 01` |
| Get position | `D1 03 01` |
| Get battery | `F0 03 01` |
| Speed slow / comfort / standard | `F2 00` / `F2 01` / `F2 02` (TS5001, TS5101, TS5200) |
| Limits: initialise, move up, move down, step up, step down, set | `21 03 01`, `CF 03 01`, `1F 03 01`, `22 03 01`, `23 03 01`, `41 03 01` |

FanBridge uses set position, stop, get position and get battery. Speed and limit setting are documented here but not exposed (use the Tuiss app for installation).

TuissESP writes the set-position bytes as "position = (open × 10) mod 256" plus a "group" byte from a banded table; that is exactly the low and high byte of `open × 10` (checked for every value 0–100), which is how FanBridge encodes it.

## Replies (notifications)

- **Position**: bytes 7–8 of the frame, little-endian, = percent open × 10.
- **Battery**: a frame with byte 4 = `D2`. Low if the frame is 7 bytes long or byte 5 ≥ 10, otherwise good.

## Matter mapping

Matter window coverings count lift as percent **closed** (`CurrentPositionLiftPercent100ths`, 0 = open, 10000 = closed). FanBridge converts: `closed100ths = (100 − open) × 100`. Google's Open / Close / Go-to-percentage set `TargetPositionLiftPercent100ths`; Stop sets target = current. FanBridge reports the current position after each command succeeds and whenever it reads the position back.

"""Reference encoder for FanLamp/LampSmart v2/v3 BLE formats (matches fan_decode.py)."""
from binascii import crc_hqx
from fan_decode import v2_whiten, v2_sign, decode_v2

# name: (prefix, with_sign, dev_type, ad_type, flags)
VARIANTS = {
    "remote":   (b"\x10\x00\x56", True,  0x0400, 0x16, 0x02),  # your physical remote
    "fl_v3":    (b"\x20\x80\x00", True,  0x0400, 0x03, 0x19),  # FanLamp Pro app (v3)
    "fl_v2":    (b"\x10\x80\x00", False, 0x0400, 0x03, 0x19),  # FanLamp Pro app (v2)
    "ls_v3":    (b"\x30\x80\x00", True,  0x0100, 0x03, 0x19),  # LampSmart Pro app (v3)
    "ls_v2":    (b"\x10\x80\x00", False, 0x0100, 0x03, 0x19),  # LampSmart Pro app (v2)
}

def encode(variant, remote_id, index, cmd, param, a0, a1, a2, tx, seed):
    prefix, with_sign, dev, ad_type, flags = VARIANTS[variant]
    base = bytearray(prefix) + bytes([tx]) + dev.to_bytes(2, "little") + remote_id.to_bytes(4, "little") \
        + bytes([index, cmd, 0, param, a0, a1, a2])
    sign = v2_sign(base[1:17], tx, seed) if with_sign else 0
    d = base + sign.to_bytes(2, "little") + b"\x00" + seed.to_bytes(2, "little")
    o = d[:2] + v2_whiten(d[2:-2], seed & 0xFF, (d[1] & 3) << 5) + d[-2:]
    svc = bytes(o + crc_hqx(bytes(o), seed ^ 0xFFFF).to_bytes(2, "little"))
    return bytes([0x02, 0x01, flags, 0x1B, ad_type, 0xF0, 0x08]) + svc

if __name__ == "__main__":
    h = lambda s: bytes.fromhex(s.replace(".", ""))
    checks = [
        # your remote's light-toggle captures
        (("remote", 0x034A5A9D, 0, 0x09, 0, 0, 0, 0, 188, 0x54C5), "02.01.02.1B.16.F0.08." "1000A26CC1067B5C9750C056C2D745272E6822B0C554D85A"),
        (("remote", 0x034A5A9D, 0, 0x09, 0, 0, 0, 0, 189, 0x6799), "02.01.02.1B.16.F0.08." "10005C02AFA2F30FE73F7C61E841A88C9D05B25A99671EF0"),
        # ha-ble-adv reference vectors: FanLamp Pro app PAIR, v3 and v2
        (("fl_v3", 0xD2135C22, 2, 0x28, 0, 0, 0, 0, 117, 0x2B53), "02.01.19.1B.03.F0.08.20.80.B8.01.E1.22.C6.F2.D3.A7.67.44.A4.9F.67.F6.B6.78.45.8B.53.2B.B9.1B"),
        (("fl_v2", 0xD2135C22, 2, 0x28, 0, 0, 0, 0, 117, 0x2B53), "02.01.19.1B.03.F0.08.10.80.B8.01.E1.22.C6.F2.D3.A7.67.44.A4.9F.67.F6.B6.A2.22.8B.53.2B.E1.B5"),
    ]
    ok = True
    for args, want in checks:
        got = encode(*args)
        m = got == h(want); ok &= m
        print("MATCH" if m else "DIFF ", args[0], got.hex().upper())
    print("ALL OK" if ok else "FAILED")

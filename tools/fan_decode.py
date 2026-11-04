"""Standalone decoder for FanLamp-style remote adverts (own implementation)."""
from binascii import crc_hqx
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

XBOXES = [
 0xB7,0xFD,0x93,0x26,0x36,0x3F,0xF7,0xCC,0x34,0xA5,0xE5,0xF1,0x71,0xD8,0x31,0x15,
 0x04,0xC7,0x23,0xC3,0x18,0x96,0x05,0x9A,0x07,0x12,0x80,0xE2,0xEB,0x27,0xB2,0x75,
 0xD0,0xEF,0xAA,0xFB,0x43,0x4D,0x33,0x85,0x45,0xF9,0x02,0x7F,0x50,0x3C,0x9F,0xA8,
 0x51,0xA3,0x40,0x8F,0x92,0x9D,0x38,0xF5,0xBC,0xB6,0xDA,0x21,0x10,0xFF,0xF3,0xD2,
 0xE0,0x32,0x3A,0x0A,0x49,0x06,0x24,0x5C,0xC2,0xD3,0xAC,0x62,0x91,0x95,0xE4,0x79,
 0xE7,0xC8,0x37,0x6D,0x8D,0xD5,0x4E,0xA9,0x6C,0x56,0xF4,0xEA,0x65,0x7A,0xAE,0x08,
 0xE1,0xF8,0x98,0x11,0x69,0xD9,0x8E,0x94,0x9B,0x1E,0x87,0xE9,0xCE,0x55,0x28,0xDF,
 0x8C,0xA1,0x89,0x0D,0xBF,0xE6,0x42,0x68,0x41,0x99,0x2D,0x0F,0xB0,0x54,0xBB,0x16]

def rev8(x):
    x = ((x & 0x55) << 1) | ((x & 0xAA) >> 1)
    x = ((x & 0x33) << 2) | ((x & 0xCC) >> 2)
    return ((x & 0x0F) << 4) | ((x & 0xF0) >> 4)

def lfsr_whiten(buf, seed):
    out, r = bytearray(), seed
    for v in buf:
        b = 0
        for j in range(8):
            r <<= 1
            if r & 0x80:
                r ^= 0x11; b |= 1 << j
            r &= 0x7F
        out.append(v ^ b)
    return out

def v2_whiten(buf, seed, salt):
    return bytearray(XBOXES[((seed + i + 9) & 0x1F) + salt] ^ seed ^ v for i, v in enumerate(buf))

def v2_sign(buf16, tx, seed):
    key = bytes([seed & 0xFF, seed >> 8, tx, 0x0D,0xBF,0xE6,0x42,0x68,0x41,0x99,0x2D,0x0F,0xB0,0x54,0xBB,0x16])
    e = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    s = int.from_bytes((e.update(bytes(buf16)) + e.finalize())[:2], "little")
    return s or 0xFFFF

def decode_v2(svc):  # svc = bytes after the F0 08 UUID
    b = bytearray(svc)
    seed = int.from_bytes(b[-4:-2], "little")
    crc_ok = crc_hqx(bytes(b[:-2]), seed ^ 0xFFFF) == int.from_bytes(b[-2:], "little")
    d = b[0:2] + v2_whiten(b[2:-4], seed & 0xFF, (b[1] & 3) << 5) + b[-4:-2]
    sign = int.from_bytes(d[17:19], "little")
    return dict(crc_ok=crc_ok, sign_ok=(sign == 0 or v2_sign(d[1:17], d[3], seed) == sign),
                prefix=d[0:3].hex(), tx=d[3], dev_type=hex(int.from_bytes(d[4:6], "little")),
                remote_id=hex(int.from_bytes(d[6:10], "little")), index=d[10],
                cmd=hex(d[11]), param=hex(d[13]), args=d[14:17].hex(), seed=hex(seed), plain=d.hex())

def decode_v1(mfr):  # mfr = full manufacturer data incl. 2-byte company id
    b = bytearray(mfr[13:])  # skip 56 55 18 87 52 + 8-byte whitened prefix
    d = bytearray(rev8(x) for x in lfsr_whiten(b, 0x0C))
    crc2_ok = int.from_bytes(d[-2:]) == 0x9372
    d = d[:-2]
    seed = int.from_bytes(d[10:12])
    crc_ok = crc_hqx(bytes(d[:12]), seed ^ 0xFFFF) == int.from_bytes(d[12:14])
    gi = int.from_bytes(d[1:3], "little")
    return dict(crc_ok=crc_ok and crc2_ok, cmd=hex(d[0]), group=hex(gi & 0xF0FF), index=(gi >> 8) & 0xF,
                args=d[3:6].hex(), tx=d[6], param=hex(d[7]), seed=hex(seed), plain=d.hex())

def decode_any(hexstr):
    """Decode one capture pasted from nRF Connect.

    Accepts either the Service Data value (UUID 0x08F0, starts with 10 00 / 20 80 ...),
    or the Manufacturer Data value including the company ID (starts with 56 55 18 87 52).
    """
    b = bytes.fromhex(hexstr.replace("0x", "").replace(" ", "").replace(".", "").replace(":", ""))
    if b[:2] == b"\xf0\x08":  # included the UUID bytes
        b = b[2:]
    if b[:5] == bytes.fromhex("5655188752"):
        return "v1 manufacturer data", decode_v1(b)
    if len(b) == 24:
        return "v2/v3 service data", decode_v2(b)
    raise ValueError(f"don't recognise a {len(b)}-byte payload")


if __name__ == "__main__":
    import sys
    if len(sys.argv) > 1:
        for h in sys.argv[1:]:
            kind, d = decode_any(h)
            print(kind, d)
        sys.exit(0)
    # No arguments: decode the captures this project was built from (light toggle x3)
    svc = ["1000A26CC1067B5C9750C056C2D745272E6822B0C554D85A",
           "10005C02AFA2F30FE73F7C61E841A88C9D05B25A99671EF0",
           "10001C1171FBF1A92478E982824EDB1CDE22C24F6941E63E"]
    mfr = ["5655188752B65F2B5E00FC3151482F9308240A06FCB86ACE1C62E06057",
           "5655188752B65F2B5E00FC3151482F9308240A86FCFD2F0C5982736057",
           "5655188752B65F2B5E00FC3151482F9308240AE6FCBC6EAB18938C6057"]
    for i, h in enumerate(svc, 1): print("svc", i, decode_v2(bytes.fromhex(h)))
    for i, h in enumerate(mfr, 1): print("mfr", i, decode_v1(bytes.fromhex(h)))

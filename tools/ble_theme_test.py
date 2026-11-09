"""Sends a custom theme to the life counter over BLE (protocol v1).

    python ble_theme_test.py [SLOT] [FILE.bin]

Without FILE it sends a synthetic test image. Needs `pip install bleak`
(works on macOS, Windows and Linux/BlueZ).
"""
import asyncio, math, struct, sys, time, zlib
from bleak import BleakClient, BleakScanner

SVC = "4d544700-6c69-6665-2d63-6f756e746572"
CTRL = "4d544701-6c69-6665-2d63-6f756e746572"
DATA = "4d544702-6c69-6665-2d63-6f756e746572"


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def make_record(name="Teste BLE"):
    # palette: 0 = black, 1..200 = blue->magenta gradient, 250 = gold ring
    pal = [0] * 256
    for i in range(1, 201):
        t = i / 200
        pal[i] = rgb565(int(40 + 160 * t), int(10 + 20 * t), int(120 - 60 * t))
    pal[250] = rgb565(220, 180, 70)
    px = bytearray(240 * 240)
    for y in range(240):
        for x in range(240):
            d = math.hypot(x - 119.5, y - 119.5)
            if d > 120:
                v = 0
            elif 106 <= d <= 111:
                v = 250
            else:
                v = 1 + int(199 * min(1, y / 239))
                if (x // 20 + y // 20) % 2 and d < 100:
                    v = max(1, v - 40)
            px[y * 240 + x] = v
    colors = [rgb565(255, 250, 235), rgb565(255, 210, 90), rgb565(140, 180, 255), rgb565(200, 170, 220),
              rgb565(40, 20, 60), rgb565(60, 255, 100), rgb565(255, 80, 80),
              rgb565(255, 240, 80), rgb565(255, 160, 0), rgb565(255, 40, 40)]
    body = struct.pack("<20s10HBbbBHHHH", name.encode()[:20], *colors, 1, 3, 3, 0,
                       rgb565(255, 40, 160), 0, 0, 0)
    body += struct.pack("<256H", *pal) + bytes(px)
    total = 12 + len(body)
    assert total == 64 + 512 + 57600, total
    crc_input = body
    crc = zlib.crc32(crc_input) & 0xFFFFFFFF
    return struct.pack("<4sII", b"MTGT", total, crc) + body


async def main(slot=0):
    dev = await BleakScanner.find_device_by_filter(lambda d, ad: SVC in [u.lower() for u in ad.service_uuids], timeout=15)
    if not dev:
        sys.exit("device not found")
    print("found", dev.address, dev.name)
    q = asyncio.Queue()
    async with BleakClient(dev) as c:
        print("mtu", c.mtu_size)
        await c.start_notify(CTRL, lambda _, d: q.put_nowait(bytes(d)))

        async def cmd(b):
            await c.write_gatt_char(CTRL, bytes(b), response=True)
            return await asyncio.wait_for(q.get(), 10)

        print("info", (await cmd([1])).hex())
        rec = open(sys.argv[2], "rb").read() if len(sys.argv) > 2 else make_record()
        t0 = time.time()
        r = await asyncio.wait_for(cmd([2, slot] + list(struct.pack("<I", len(rec)))), 20)
        print("begin", r.hex(), f"{time.time() - t0:.1f}s")
        chunk = min(c.mtu_size - 3, 240)
        acked, sent, window = 0, 0, 8192
        while acked < len(rec):
            while sent < len(rec) and sent - acked < window:
                await c.write_gatt_char(DATA, rec[sent:sent + chunk], response=False)
                sent += len(rec[sent:sent + chunk])
            r = await asyncio.wait_for(q.get(), 10)
            if r[0] == 3:
                acked = struct.unpack("<I", r[1:5])[0]
            else:
                sys.exit("error " + r.hex())
        dt = time.time() - t0
        print(f"sent {len(rec)} bytes in {dt:.1f}s ({len(rec) / dt / 1024:.1f} KB/s)")
        print("end", (await cmd([4])).hex())
        print("select", (await cmd([5, 6 + slot])).hex())
        print("info", (await cmd([1])).hex())


if __name__ == "__main__":   # importing (e.g. for make_record) must not upload anything
    asyncio.run(main(int(sys.argv[1]) if len(sys.argv) > 1 else 0))

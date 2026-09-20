import hid
import struct
import sys
import time

VID, PID = 0x3941, 0xAF50
REPORT_LEN = 64


def hx(b, n=None):
    b = bytes(b)
    if n:
        b = b[:n]
    return ' '.join('%02x' % x for x in b)


def find_devices():
    return [d for d in hid.enumerate() if d['vendor_id'] == VID and d['product_id'] == PID]


def try_variant(dev, label, mk):
    print('== variant %s ==' % label)

    def send(cmd):
        buf = mk(cmd)
        try:
            n = dev.write(buf)
            print('   wrote %d bytes -> %s' % (len(buf), n))
            return True
        except Exception as e:
            print('   write failed: %s' % e)
            return False

    def drain(seconds, tag, maxprint=10):
        t0 = time.time()
        n = 0
        while time.time() - t0 < seconds:
            data = dev.read(REPORT_LEN, timeout_ms=100)
            if data:
                n += 1
                if n <= maxprint:
                    print('   [%s] %dB  %s' % (tag, len(data), hx(data)))
        print('   [%s] %d reports' % (tag, n))
        return n

    if drain(1.0, 'idle before cmds') > 0:
        print('   (device streams without commands)')

    print('   send 66 00 (device info)')
    send(0x00)
    n1 = drain(1.5, 'after 66 00')

    print('   send 66 3c (calibration)')
    send(0x3c)
    n2 = drain(1.5, 'after 66 3c')

    print('   send 66 01 (stream on)')
    send(0x01)

    t0 = time.time()
    count = 0
    decoded = 0
    while time.time() - t0 < 5.0:
        data = dev.read(REPORT_LEN, timeout_ms=200)
        if not data:
            continue
        count += 1
        b = bytes(data)
        if len(b) >= 60 and b[0] == 0x99 and b[1] == 0x65:
            ax, ay, az = struct.unpack_from('<fff', b, 4)
            gx, gy, gz = struct.unpack_from('<fff', b, 16)
            (temp,) = struct.unpack_from('<f', b, 28)
            mx, my = struct.unpack_from('<ff', b, 32)
            (tick,) = struct.unpack_from('<I', b, 40)
            (mz,) = struct.unpack_from('<f', b, 52)
            if decoded < 10 or decoded % 200 == 0:
                print('   sample %4d: acc=(%7.3f,%7.3f,%7.3f) gyr=(%8.2f,%8.2f,%8.2f) mag=(%8.1f,%8.1f,%8.1f) T=%.1f tick=%d'
                      % (decoded, ax, ay, az, gx, gy, gz, mx, my, mz, temp, tick))
            decoded += 1
        elif count <= 8:
            print('   [stream] %dB %s' % (len(b), hx(b)))

    print('   stream: %d reports, %d decoded as 99 65 (%.1f Hz)' % (count, decoded, decoded / 5.0))
    return n1 + n2 + decoded


def main():
    ds = find_devices()
    print('found %d RayNeo HID device(s)' % len(ds))
    if not ds:
        return 1
    for d in ds:
        for k in sorted(d.keys()):
            print('   %s: %r' % (k, d[k]))

    dev = hid.device()
    dev.open_path(ds[0]['path'])
    try:
        dev.set_nonblocking(False)
    except Exception as e:
        print('set_nonblocking failed: %s' % e)
    print('opened first device OK')

    total = try_variant(dev, 'A: raw 64B, 0x66 at [0]', lambda c: bytes([0x66, c] + [0] * 62))
    if total == 0:
        total = try_variant(dev, 'B: 65B, report-id 0x00 then 0x66', lambda c: bytes([0x00, 0x66, c] + [0] * 61))

    dev.close()
    print('done (total activity: %d)' % total)
    return 0


if __name__ == '__main__':
    sys.exit(main())

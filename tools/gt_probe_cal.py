import hid
import sys
import time

VID, PID = 0x3941, 0xAF50
STREAM_TAG = (0x99, 0x65)


def hx(b):
    return ' '.join('%02x' % x for x in bytes(b))


def main():
    ds = [d for d in hid.enumerate() if d['vendor_id'] == VID and d['product_id'] == PID]
    if not ds:
        print('no device found')
        return 1
    dev = hid.device()
    dev.open_path(ds[0]['path'])

    def send(cmd):
        dev.write(bytes([0x00, 0x66, cmd] + [0] * 61))

    def drain(seconds, tag, maxprint=24):
        t0 = time.time()
        samples = 0
        others = 0
        while time.time() - t0 < seconds:
            d = dev.read(64, 100)
            if not d:
                continue
            b = bytes(d)
            if len(b) >= 2 and (b[0], b[1]) == STREAM_TAG:
                samples += 1
                continue
            others += 1
            if others <= maxprint:
                print('  [%s] non-stream %dB  %s' % (tag, len(b), hx(b)))
        print('  [%s] %d stream samples, %d other reports' % (tag, samples, others))
        return others

    drain(0.5, 'pre')
    print('send 66 02 (stream off)')
    send(0x02)
    drain(1.0, 'after 66 02')

    print('send 66 3c (factory transform + gyro bias)')
    send(0x3c)
    drain(4.0, '66 3c reply')

    print('send 66 3e (81-point gyro temperature bias table)')
    send(0x3e)
    drain(4.0, '66 3e reply')

    print('send 66 00 (device info)')
    send(0x00)
    drain(2.0, '66 00 reply')

    dev.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())

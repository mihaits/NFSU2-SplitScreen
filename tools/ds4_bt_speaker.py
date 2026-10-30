"""Plays a sound on a DualShock 4's speaker over Bluetooth (experiment for per-player audio).

The speaker takes SBC frames in HID output report 0x17 (462 bytes, 4 frames) or 0x14 (270 bytes, 2 frames). Layout as
PadForge documents it (padforge.org/docs/reference/controller-audio-internals; not checked against a capture):
  [0] id  [1] 0x40  [2] 0xa0  [3..4] frame counter, little-endian, += frames per report
  [5] 0x02 speaker (0x24 headphone jack)  [6..] SBC frames  last 4 bytes: CRC-32 of 0xa2 + the report
SBC: 32 kHz, 8 subbands, 16 blocks, joint stereo, bitpool 48: 109-byte frames of 128 samples per channel (4 ms). So one
0x17 report is due every 16 ms. Output report 0x11 enables the speaker first: flag 0x80 at [3], speaker volume at [24].

ffmpeg encodes the input: plain stereo and loudness allocation, 108 bytes at bitpool 48. The header says so, the pad
should follow it.

  python ds4_bt_speaker.py                   # 4 s 440 Hz tone on the first Bluetooth DS4
  python ds4_bt_speaker.py --list            # list Bluetooth DS4 pads
"""
import argparse, ctypes as C, ctypes.wintypes as W, subprocess, sys, time

from ds_bt_speaker import bt_dualsenses, crc, k32, log, open_hid, winmm

PIDS = ('05c4', '09cc')
SIZES = {0x14: (270, 2), 0x17: (462, 4)}      # report id: length, SBC frames
FRAME_SAMPLES = 128
RATE = 32000


def write(h, report):
    n = W.DWORD()
    buf = (C.c_ubyte * len(report)).from_buffer_copy(report)
    if not k32.WriteFile(h, buf, len(report), C.byref(n), None):
        raise OSError(f'WriteFile failed, error {C.get_last_error()}')


def enable_report(volume):
    r = bytearray(78)
    r[0], r[1], r[2], r[3] = 0x11, 0xC0, 0xA0, 0x80
    r[24] = volume
    r[74:78] = crc(r, 74)
    return bytes(r)


def audio_report(rid, counter, frames, path):
    size = SIZES[rid][0]
    r = bytearray(size)
    r[0], r[1], r[2] = rid, 0x40, 0xA0
    r[3], r[4] = counter & 0xFF, counter >> 8 & 0xFF
    r[5] = path
    data = b''.join(frames)
    r[6:6 + len(data)] = data
    r[size - 4:size] = crc(r, size - 4)
    return bytes(r)


def sbc_frames(data):
    """Splits a raw SBC stream, using the size of the first frame's header (CBR)."""
    if len(data) < 4 or data[0] != 0x9C:
        sys.exit(f'not an SBC stream ({len(data)} bytes)')
    b1, bitpool = data[1], data[2]
    blocks, stereo_mode, subbands = ((b1 >> 4 & 3) + 1) * 4, b1 >> 2 & 3, 8 if b1 & 1 else 4
    channels = 1 if stereo_mode == 0 else 2
    joint = stereo_mode == 3
    bits = (subbands if joint else 0) + blocks * bitpool * (channels if stereo_mode in (0, 1) else 1)
    size = 4 + (4 * subbands * channels + 7) // 8 + (bits + 7) // 8
    log(f'SBC: {blocks} blocks, {subbands} subbands, channel mode {stereo_mode}, allocation {b1 >> 1 & 1}, '
        f'bitpool {bitpool}, {size} bytes a frame')
    return [data[i:i + size] for i in range(0, len(data) - size + 1, size)]


def encode(args):
    src = (['-i', args.file] if args.file else
           ['-f', 'lavfi', '-i', f'sine=frequency={args.freq}:sample_rate=32000:duration={args.seconds}'])
    cmd = (['ffmpeg', '-hide_banner', '-loglevel', 'error'] + src +
           ['-af', f'volume={args.gain}', '-ar', '32000', '-ac', '2', '-c:a', 'sbc', '-b:a', f'{args.kbps}k',
            '-f', 'sbc', '-'])
    return sbc_frames(subprocess.run(cmd, capture_output=True, check=True).stdout)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--pad', type=int, default=0, help='index in --list')
    ap.add_argument('--file')
    ap.add_argument('--freq', type=int, default=440)
    ap.add_argument('--seconds', type=float, default=4)
    ap.add_argument('--gain', type=float, default=0.5)
    ap.add_argument('--kbps', type=int, default=216)
    ap.add_argument('--volume', type=lambda s: int(s, 0), default=0x4F)
    ap.add_argument('--report', type=lambda s: int(s, 0), default=0x17, choices=SIZES)
    ap.add_argument('--path', type=lambda s: int(s, 0), default=0x02, help='0x02 speaker, 0x24 headphone jack')
    args = ap.parse_args()

    pads = bt_dualsenses(PIDS)
    for i, (p, s) in enumerate(pads):
        log(f'pad {i}: serial ..{s[-5:]}  {p[:60]}')
    if args.list:
        return
    if not pads:
        sys.exit('no Bluetooth DS4 found (on, unplugged, DS4Windows stopped?)')

    log('encoding with ffmpeg')
    frames = encode(args)
    per_report = SIZES[args.report][1]
    if 6 + per_report * len(frames[0]) > SIZES[args.report][0] - 4:
        sys.exit(f'{len(frames[0])}-byte frames do not fit in report 0x{args.report:02x}')
    period = per_report * FRAME_SAMPLES / RATE
    log(f'{len(frames)} SBC frames ({len(frames) * FRAME_SAMPLES / RATE:.2f} s), 0x{args.report:02x} every '
        f'{period * 1000:.1f} ms')

    h = open_hid(pads[args.pad][0])
    if not h:
        sys.exit(f'open failed, error {C.get_last_error()}')
    winmm.timeBeginPeriod(1)
    try:
        write(h, enable_report(args.volume))
        log('sent 0x11 speaker enable')
        time.sleep(0.05)
        counter = sent = late = 0
        nxt = time.perf_counter()
        log('streaming')
        for k in range(0, len(frames) - per_report + 1, per_report):
            write(h, audio_report(args.report, counter, frames[k:k + per_report], args.path))
            counter = (counter + per_report) & 0xFFFF
            sent += 1
            nxt += period
            d = nxt - time.perf_counter()
            if d > 0.002:
                time.sleep(d - 0.0015)
            while time.perf_counter() < nxt:
                pass
            late += d < 0
            if sent % 31 == 0:
                log(f'sent {sent} reports, {late} late')
        log(f'done: {sent} reports, {late} late')
    finally:
        winmm.timeEndPeriod(1)
        k32.CloseHandle(h)


if __name__ == '__main__':
    main()

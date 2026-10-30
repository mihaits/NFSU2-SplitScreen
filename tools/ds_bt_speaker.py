"""Plays a sound on a DualSense's speaker over Bluetooth (experiment for per-player audio).

Windows has no audio device for a Bluetooth DualSense; the speaker takes Opus frames in HID output report 0x39
(547 bytes, the pad's largest output report and the length Windows' HID class wants for every write). Layout as in
awalol/DS5Dongle and Myoko/KytyPS5 (dualSenseBluetooth.cpp):
  [0] 0x39  [1] seq << 4
  [2] 0x91 config sub-packet, len 6: [4] 0x7e enable mask (bit 0 clear: no mic stream back), [5..8] 48, [9] counter += 2
  [10] 0xd2 haptics, len 64: [12..75] int8 PCM at 3 kHz (silent here)
  [140] 0xd3 speaker, len 200: [142..341], [342..541] two 480-frame Opus packets (48 kHz stereo, CBR 160 kbit/s)
  [543..546] CRC-32 of 0xa2 + report[0..542]
One report every 21.333 ms. The pad plays 960 samples per report, i.e. 45 kHz, so the audio is resampled to 45 kHz and
then labelled 48 kHz before encoding. Output report 0x31 (audio_control = speaker path) routes it to the speaker.

ffmpeg (libopus) encodes the input; no opus library is needed here.

  python ds_bt_speaker.py                    # 4 s 440 Hz tone on the first Bluetooth DualSense
  python ds_bt_speaker.py --file x.wav       # any file ffmpeg reads
  python ds_bt_speaker.py --list             # list Bluetooth DualSenses
"""
import argparse, ctypes as C, ctypes.wintypes as W, subprocess, sys, time, zlib

hid = C.WinDLL('hid')
sa = C.WinDLL('setupapi')
k32 = C.WinDLL('kernel32', use_last_error=True)
winmm = C.WinDLL('winmm')

BT_HID = '00001124-0000-1000-8000-00805f9b34fb'
REPORT = 547
OPUS_BYTES = 200
PERIOD = 64 / 3000  # 21.333 ms


class GUID(C.Structure):
    _fields_ = [('a', W.DWORD), ('b', W.WORD), ('c', W.WORD), ('d', C.c_ubyte * 8)]


class IFD(C.Structure):
    _fields_ = [('cbSize', W.DWORD), ('guid', GUID), ('flags', W.DWORD), ('res', C.c_void_p)]


class CAPS(C.Structure):
    _fields_ = [('Usage', W.USHORT), ('UsagePage', W.USHORT), ('In', W.USHORT), ('Out', W.USHORT),
                ('Feat', W.USHORT), ('r', W.USHORT * 17), ('x', W.USHORT * 10)]


sa.SetupDiGetClassDevsW.restype = C.c_void_p
sa.SetupDiEnumDeviceInterfaces.argtypes = [C.c_void_p, C.c_void_p, C.POINTER(GUID), W.DWORD, C.POINTER(IFD)]
sa.SetupDiGetDeviceInterfaceDetailW.argtypes = [C.c_void_p, C.POINTER(IFD), C.c_void_p, W.DWORD,
                                                 C.POINTER(W.DWORD), C.c_void_p]
sa.SetupDiDestroyDeviceInfoList.argtypes = [C.c_void_p]
k32.CreateFileW.restype = W.HANDLE
k32.WriteFile.argtypes = [W.HANDLE, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.c_void_p]
hid.HidD_GetSerialNumberString.argtypes = [W.HANDLE, C.c_void_p, W.ULONG]

T0 = time.perf_counter()


def log(msg):
    print(f'[{time.perf_counter() - T0:6.2f}] {msg}', flush=True)


def bt_dualsenses(pids=('0ce6', '0df2')):
    """(path, serial) of every Bluetooth DualSense HID interface (or other Sony pids)."""
    g = GUID()
    hid.HidD_GetHidGuid(C.byref(g))
    hdi = sa.SetupDiGetClassDevsW(C.byref(g), None, None, 0x12)
    out, i = [], 0
    while True:
        d = IFD()
        d.cbSize = C.sizeof(IFD)
        if not sa.SetupDiEnumDeviceInterfaces(hdi, None, C.byref(g), i, C.byref(d)):
            break
        i += 1
        need = W.DWORD()
        sa.SetupDiGetDeviceInterfaceDetailW(hdi, C.byref(d), None, 0, C.byref(need), None)
        buf = C.create_string_buffer(need.value)
        C.cast(buf, C.POINTER(W.DWORD))[0] = 8
        sa.SetupDiGetDeviceInterfaceDetailW(hdi, C.byref(d), buf, need, None, None)
        path = C.wstring_at(C.addressof(buf) + 4)
        low = path.lower()
        if BT_HID in low and 'vid&0002054c' in low and any(f'pid&{p}' in low for p in pids):
            h = open_hid(path)
            if h:
                s = C.create_unicode_buffer(64)
                hid.HidD_GetSerialNumberString(h, s, C.sizeof(s))
                k32.CloseHandle(h)
                out.append((path, s.value))
    sa.SetupDiDestroyDeviceInfoList(hdi)
    return out


def open_hid(path):
    h = k32.CreateFileW(path, 0xC0000000, 3, None, 3, 0, None)
    return None if h in (None, W.HANDLE(-1).value) else W.HANDLE(h)


def write(h, report):
    n = W.DWORD()
    buf = (C.c_ubyte * REPORT).from_buffer_copy(report.ljust(REPORT, b'\0'))
    if not k32.WriteFile(h, buf, REPORT, C.byref(n), None):
        raise OSError(f'WriteFile failed, error {C.get_last_error()}')


def crc(report, end):
    return zlib.crc32(report[:end], zlib.crc32(b'\xa2')).to_bytes(4, 'little')


def speaker_route(volume):
    """Output report 0x31: speaker volume + audio path speaker + preamp, as DS5_Bridge sends it."""
    r = bytearray(78)
    r[0], r[1], r[2] = 0x31, 0x00, 0x10
    c = 3
    r[c + 0] = 0x80 | 0x20  # valid_flag0: audio_control, speaker_volume
    r[c + 1] = 0x80         # valid_flag1: audio_control2
    r[c + 5] = volume       # speaker_volume (0x64 = max)
    r[c + 7] = 0x30         # audio_control: output path speaker
    r[c + 37] = 0x02        # audio_control2: speaker preamp gain
    r[74:78] = crc(r, 74)
    return bytes(r)


def audio_report(seq, counter, opus):
    r = bytearray(REPORT)
    r[0], r[1] = 0x39, (seq & 15) << 4
    r[2], r[3], r[4] = 0x91, 6, 0x7e
    r[5:9] = bytes([48] * 4)
    r[9] = counter & 0xFF
    r[10], r[11] = 0xd2, 64
    r[140], r[141] = 0xd3, OPUS_BYTES
    r[142:142 + 2 * OPUS_BYTES] = opus
    r[543:547] = crc(r, 543)
    return bytes(r)


def ogg_packets(data):
    """Packets of an Ogg stream (the Opus header and tags packets included)."""
    pkts, cur, i = [], b'', 0
    while i + 27 <= len(data) and data[i:i + 4] == b'OggS':
        nseg = data[i + 26]
        lace = data[i + 27:i + 27 + nseg]
        j = i + 27 + nseg
        for v in lace:
            cur += data[j:j + v]
            j += v
            if v < 255:
                pkts.append(cur)
                cur = b''
        i = j
    return pkts


def encode(args):
    src = (['-i', args.file] if args.file else
           ['-f', 'lavfi', '-i', f'sine=frequency={args.freq}:sample_rate=48000:duration={args.seconds}'])
    cmd = (['ffmpeg', '-hide_banner', '-loglevel', 'error'] + src +
           ['-af', f'volume={args.gain},aresample=45000,asetrate=48000', '-ac', '2',
            '-c:a', 'libopus', '-application', 'audio', '-frame_duration', '10', '-b:a', '160k', '-vbr', 'off',
            '-compression_level', '0', '-f', 'ogg', '-'])
    data = subprocess.run(cmd, capture_output=True, check=True).stdout
    pkts = ogg_packets(data)[2:]  # skip OpusHead, OpusTags
    bad = {len(p) for p in pkts if len(p) != OPUS_BYTES}
    if bad:
        # Hard CBR should make every packet 200 bytes; a short one is zero-padded below, which may click.
        log(f'warning: packet sizes other than {OPUS_BYTES}: {sorted(bad)[:8]}')
    pkts = [p[:OPUS_BYTES].ljust(OPUS_BYTES, b'\0') for p in pkts]
    if len(pkts) % 2:
        pkts.append(pkts[-1])
    return pkts


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--pad', type=int, default=0, help='index in --list')
    ap.add_argument('--file')
    ap.add_argument('--freq', type=int, default=440)
    ap.add_argument('--seconds', type=float, default=4)
    ap.add_argument('--gain', type=float, default=0.5)
    ap.add_argument('--volume', type=lambda s: int(s, 0), default=0x64)
    ap.add_argument('--no-route', action='store_true', help="don't send the 0x31 speaker route")
    args = ap.parse_args()

    pads = bt_dualsenses()
    for i, (p, s) in enumerate(pads):
        log(f'pad {i}: serial {s}  {p}')
    if args.list:
        return
    if not pads:
        sys.exit('no Bluetooth DualSense found (connected and not hidden by HidHide?)')

    log('encoding with ffmpeg/libopus')
    pkts = encode(args)
    log(f'{len(pkts)} Opus packets ({len(pkts) * 10 / 1000 * 48 / 45:.2f} s)')

    h = open_hid(pads[args.pad][0])
    if not h:
        sys.exit(f'open failed, error {C.get_last_error()}')
    winmm.timeBeginPeriod(1)
    try:
        if not args.no_route:
            write(h, speaker_route(args.volume))
            log('sent 0x31 speaker route')
            time.sleep(0.05)
        seq = counter = 0
        nxt = time.perf_counter()
        late = 0
        log('streaming')
        for k in range(0, len(pkts), 2):
            counter += 2
            write(h, audio_report(seq, counter, pkts[k] + pkts[k + 1]))
            seq += 1
            nxt += PERIOD
            d = nxt - time.perf_counter()
            if d > 0.002:
                time.sleep(d - 0.0015)
            while time.perf_counter() < nxt:
                pass
            if d < 0:
                late += 1
            if k % 94 == 0:
                log(f'sent {k // 2 + 1} reports, {late} late')
        log(f'done: {len(pkts) // 2} reports, {late} late')
    finally:
        winmm.timeEndPeriod(1)
        k32.CloseHandle(h)


if __name__ == '__main__':
    main()

"""Decodes sbc_test.exe's output with ffmpeg and prints the SNR against its input (best delay and gain)."""
import subprocess, sys

import numpy as np

base = sys.argv[1] if len(sys.argv) > 1 else 'build/sbc_test'
ref = np.fromfile(base + '.raw', dtype='<f4').astype(float)
out = subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'warning', '-f', 'sbc', '-i', base + '.sbc',
                      '-f', 'f32le', '-ac', '1', '-'], capture_output=True)
if out.stderr:
    print(out.stderr.decode(errors='replace')[:400])
dec = np.frombuffer(out.stdout, dtype='<f4').astype(float)
print(f'{len(ref)} input samples, {len(dec)} decoded')
best = None
for delay in range(0, 200):
    n = min(len(dec) - delay, len(ref)) - 2000
    a, b = ref[1000:n], dec[1000 + delay:n + delay]
    gain = a.dot(b) / max(b.dot(b), 1e-12)
    err = a - gain * b
    snr = 10 * np.log10(a.dot(a) / max(err.dot(err), 1e-12))
    if best is None or snr > best[0]:
        best = (snr, delay, gain)
print(f'SNR {best[0]:.1f} dB at delay {best[1]}, gain {best[2]:.3f}')

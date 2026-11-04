"""Build one generic human-head HRTF from the 96 measured HUTUBS subjects.

Per direction and ear: average the log magnitude over subjects, take the median
excess-phase delay (the pure-delay part of each subject's response), and rebuild a
minimum-phase filter plus that delay. Writes a compact binary and a validation report.
"""
import glob, os, struct, sys
import h5py, numpy as np

import sys
if len(sys.argv) != 3:
    sys.exit('usage: build_hrtf.py <folder with the HUTUBS pp*_HRIRs_measured.sofa files> <output .svhr>')
SRC, OUT = sys.argv[1], sys.argv[2]
NFFT = 2048
TAPS = 256

files = sorted(glob.glob(os.path.join(SRC, 'pp*_HRIRs_measured.sofa')),
               key=lambda p: int(os.path.basename(p).split('_')[0][2:]))
assert len(files) == 96, len(files)

irs, grid, fs = [], None, None
for p in files:
    with h5py.File(p, 'r') as f:
        sp = f['SourcePosition'][()]
        if grid is None:
            grid = sp
            fs = float(f['Data.SamplingRate'][()][0])
        else:
            assert np.allclose(grid, sp), p
        assert float(f['Data.SamplingRate'][()][0]) == fs
        irs.append(f['Data.IR'][()])
irs = np.stack(irs)                       # (S, D, 2, 256)
S, D, _, T = irs.shape
print('subjects', S, 'directions', D, 'taps', T, 'fs', fs)

H = np.fft.rfft(irs, NFFT, axis=-1)       # (S, D, 2, NFFT/2+1)
mag = np.abs(H) + 1e-12
logm = np.log(mag)

def minphase_from_logmag(lm):
    """Cepstral minimum phase spectrum from a one-sided log magnitude."""
    full = np.concatenate([lm, lm[..., -2:0:-1]], axis=-1)
    cep = np.fft.ifft(full, axis=-1).real
    w = np.zeros(NFFT)
    w[0] = 1
    w[1:NFFT // 2] = 2
    w[NFFT // 2] = 1
    return np.exp(np.fft.fft(cep * w, axis=-1))[..., :NFFT // 2 + 1]

# Excess-phase delay of every response: slope of arg(H / Hmin) over 150..1600 Hz.
Hmin = minphase_from_logmag(logm)
freqs = np.arange(NFFT // 2 + 1) * fs / NFFT
om = 2 * np.pi * freqs / fs               # rad / sample
band = (freqs >= 150) & (freqs <= 1600)
ex = H / Hmin
ph = np.unwrap(np.angle(ex), axis=-1)
wts = mag[..., band]
tau = -(wts * ph[..., band] * om[band]).sum(-1) / (wts * om[band] ** 2).sum(-1)   # samples
print('delay samples: min %.2f max %.2f' % (tau.min(), tau.max()))

# Generic response.
gl = logm.mean(0)                          # (D, 2, bins) mean log magnitude
gdelay = np.median(tau, axis=0)            # (D, 2) samples
# Only the interaural difference is trustworthy: the common part varies by ~1 ms between directions
# (measurement alignment), far more than head geometry allows. Keep +-ITD/2 about the head centre
# plus a constant so every delay is >= 0; the engine models distance itself.
c = np.abs(gdelay[:, 1] - gdelay[:, 0]).max() / 2
mid = gdelay.mean(1, keepdims=True)
gdelay = gdelay - mid + c
print('delay offset constant %.2f samples; delays %.2f..%.2f' % (c, gdelay.min(), gdelay.max()))
Gmin = minphase_from_logmag(gl)
gir = np.fft.irfft(Gmin, NFFT, axis=-1)[..., :TAPS]
# The minimum-phase response is causal and decays; fade the last 32 taps so truncation cannot click.
fade = np.ones(TAPS)
fade[-32:] = 0.5 * (1 + np.cos(np.linspace(0, np.pi, 32)))
gir = gir * fade
print('generic taps energy in last 32: %.3g' % ((gir[..., -32:] ** 2).sum() / (gir ** 2).sum()))

# Validation: spread of subjects around the generic head vs each other.
def dev(a, b):                             # rms log-magnitude difference in dB, 200 Hz..16 kHz
    sel = (freqs >= 200) & (freqs <= 16000)
    return np.sqrt(((20 * np.log10(a[..., sel] / b[..., sel])) ** 2).mean(-1))

gmag = np.abs(np.fft.rfft(gir, NFFT, axis=-1))
d_gen = dev(mag, gmag[None])               # (S, D, 2)
mean_subject = np.exp(gl)                  # same as gmag up to truncation
rng = np.random.default_rng(1)
pair = rng.permutation(S)
d_pair = dev(mag, mag[pair])
print('rms dB vs generic: mean %.2f  (subject vs subject: mean %.2f)' % (d_gen.mean(), d_pair.mean()))
print('worst subject vs generic: %.2f dB' % d_gen.mean((1, 2)).max())
itd = (gdelay[:, 1] - gdelay[:, 0]) / fs * 1e6
print('generic ITD us: min %.0f max %.0f (az 90 el 0: %s)' % (itd.min(), itd.max(), itd[(grid[:, 0] == 90) & (grid[:, 1] == 0)]))
print('subject ITD spread at az 90 el 0 (us): median %.0f  p5 %.0f  p95 %.0f' % tuple(
    np.percentile(((tau[:, (grid[:, 0] == 90) & (grid[:, 1] == 0), 1] - tau[:, (grid[:, 0] == 90) & (grid[:, 1] == 0), 0]) / fs * 1e6), [50, 5, 95])))
print('level at front (az0 el0) generic %.1f dB, subject median %.1f dB' % (
    20 * np.log10(np.sqrt((gir[(grid[:, 0] == 0) & (grid[:, 1] == 0)] ** 2).sum())),
    np.median(20 * np.log10(np.sqrt((irs[:, (grid[:, 0] == 0) & (grid[:, 1] == 0)] ** 2).sum(-1))))))

# Binary: 'SVHR', version, fs, dirs, taps, then az[D], el[D], delay[D][2], ir[D][2][taps] (float32 LE).
with open(OUT, 'wb') as f:
    f.write(b'SVHR')
    f.write(struct.pack('<IfII', 1, fs, D, TAPS))
    f.write(grid[:, 0].astype('<f4').tobytes())
    f.write(grid[:, 1].astype('<f4').tobytes())
    f.write(gdelay.astype('<f4').tobytes())
    f.write(gir.astype('<f4').tobytes())
print('wrote', OUT, os.path.getsize(OUT), 'bytes')

# Generic head for the 3D stage

`hrtf_generic.svhr` is one generic human head, built from the 96 measured subjects of the HUTUBS
database and embedded in the driver and the Configurator. The stage uses it for the direct sound.

HUTUBS: Brinkmann, Dinakaran, Pelzer, Grosche, Voss, Weinzierl and others, Audio Communication Group,
TU Berlin (with Huawei Munich Research Center and Sennheiser). Licence: CC BY 4.0,
https://creativecommons.org/licenses/by/4.0/ . The table here is a derived work (averaged and
re-encoded); the original data is at https://depositonce.tu-berlin.de .

## How it was made

`build_hrtf.py <folder of pp*_HRIRs_measured.sofa> <out.svhr>` (needs h5py, numpy):

1. Per direction and ear, the log magnitude spectra of all subjects are averaged.
2. Each response's pure-delay part (the slope of its excess phase) is estimated; the median over
   subjects is kept only as the difference between the ears (+-ITD/2). The common part varies by
   about 1 ms between directions in the data (measurement alignment), more than head geometry
   allows, so it is dropped.
3. Each filter is rebuilt as minimum phase (256 taps at 44.1 kHz).

Against single subjects the result is 4.9 dB rms away on average (two subjects are 6.9 dB apart),
and its interaural delay at 90 degrees is 654 us (subjects: median 655 us).

## Format (little-endian)

`'SVHR'`, u32 version (1), f32 rate, u32 directions D, u32 taps T, then f32 arrays: azimuth[D]
(degrees, towards the left), elevation[D], delay[D][2] (samples, ear 0 = left), ir[D][2][T].
`render/StageHrtf.h` reads it.

Fixtures copied unchanged from `Проводка2k_062` for reproducible offline tests:

- `KIH5_4F.dat`: legacy 3887-tap FIR coefficients.
- `legacy.smp`: `tek/smp/26-06-26 15-44-56.smp`, including its original header.

The playback test verifies transport/format parsing, not agreement of decoded
telemetry with the legacy application.

- `downhole_16hz.smp`: `tek/smp/24-08-23 10-35-20.smp`, used for synchronization and nonblocking configuration regression tests.

- `format13_16hz.i32`: live USO32 capture on 2026-11-27 (firmware 3.1.0,
  V2, COM10, reported 400 samples/s, gain x1, MCU Butterworth LPF and 0.1 Hz HPF).
  56100 signed little-endian int32 samples without a header; three complete
  format-13 frames, ncod=0, with one-vector pauses at slots 1/11/21/30.
  Expected codes were independently checked by Python template correlation.
  This fixture verifies framing and codes, not validity of physical sensor readings.

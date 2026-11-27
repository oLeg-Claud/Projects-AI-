Fixtures copied unchanged from `Проводка2k_062` for reproducible offline tests:

- `KIH5_4F.dat`: legacy 3887-tap FIR coefficients.
- `legacy.smp`: `tek/smp/26-06-26 15-44-56.smp`, including its original header.

The playback test verifies transport/format parsing, not agreement of decoded
telemetry with the legacy application.

- `downhole_16hz.smp`: `tek/smp/24-08-23 10-35-20.smp`, used for synchronization and nonblocking configuration regression tests.

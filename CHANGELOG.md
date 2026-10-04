# Change log

## 0.1.0-dev — publication preparation

- Project name and Bluetooth name changed to Make2Hear.
- Added pinned ESP-IDF revision and SBC source-integrity guard.
- Added reproducible defaults, ignored generated files, and CI for both modes.
- Added menuconfig settings for microphone wiring/slot, name, gain, PIN and logs.
- Fresh-build gain is 2×; the earlier prototype used 8×.
- Removed automatic AVRCP volume increases.
- Fixed stale discovery names; accepted the separate device-name property.
- Declared headless pairing; unsupported passkey input is rejected explicitly.
- Added startup/API/event-delivery checks and nonblocking timer dispatch.
- Audio starts only after the required mono SBC configuration is confirmed.
- Remote suspension returns media state to idle for heartbeat recovery.
- Added portable setup, diagnostic, licensing and contribution documentation.

The existing INMP441 task, PCM ring, 100 Hz HPF, fixed gain, limiter, mono format,
and latency patch are retained. Hardware validation of this release is pending.

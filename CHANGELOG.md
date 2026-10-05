# Change log

## 2026-10-05 — GPLv3 licensing

- Changed the project and original Make2Hear contributions to GNU GPL version 3
  only (`GPL-3.0-only`).
- Preserved Espressif's notices and the Apache-2.0, Unlicense and CC0 license texts.
- Updated source/generated-source notices, README and contribution terms.
- Documented corresponding-source requirements to review before distributing
  firmware containing the precompiled ESP32 controller and PHY libraries.
- Earlier Apache-2.0 distributions retain the permissions already granted.
- Firmware logic and configuration are unchanged.

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

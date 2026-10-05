# Release validation

## Current evidence — 2026-10-04

Version: **0.1.0-dev**. SDK: `fff9895c82d744c7237be8847347bdd1b07c6643`
(ESP-IDF 6.1), unmodified installed SDK. Host: macOS.

- Clean low-latency (`ON`) build: passed, using a newly generated configuration.
- Clean stock-batching (`OFF`) build: passed, using a separate new configuration.
- Public-default and source/cadence coupling checks passed in both modes.
- Both configurations use the shared defaults: B3, left slot, GPIO 26/25/33,
  2× gain, 100 Hz FreeRTOS tick, 160 MHz CPU, Bluetooth on core 0.
- CI is configured to reproduce both builds on Linux. A hosted GitHub Actions
  run has not occurred yet; no remote repository has been published by this work.
- No board was flashed during publication preparation. The earlier B3 prototype
  worked, but that does not establish results for the changes in this release.
- No measured end-to-end latency, precise development-board model, or hardware
  photograph is available for this release yet. Record these before claiming a
  tested hardware combination.

## Repeatable fresh-configuration builds

Activate the pinned SDK and run from a fresh checkout:

```sh
idf.py -B build-release-on -DSDKCONFIG="$PWD/build-release-on/sdkconfig" \
  -DIDF_TARGET=esp32 -DB3_LOW_LATENCY_A2DP=ON build
python3 tools/check_build.py build-release-on ON
idf.py -B build-release-off -DSDKCONFIG="$PWD/build-release-off/sdkconfig" \
  -DIDF_TARGET=esp32 -DB3_LOW_LATENCY_A2DP=OFF build
python3 tools/check_build.py build-release-off OFF
```

Use new build directories to repeat a genuinely fresh test. These commands keep
the root `sdkconfig` unchanged. The validator intentionally requires public
defaults; it will reject a deliberately customized gain, name or wiring.

## Hardware checklist — pending

Record firmware revision, exact board/module, power source, INMP441 module,
headphone model, SDK commit, gain, wiring and latency mode for each run.

- [ ] First pairing after clearing both devices' pairing records.
- [ ] Reconnection after ESP32 reboot and after headphone power off/on.
- [ ] Headphone volume up/down changes volume without a firmware-induced increase.
- [ ] Normal speech at 10/30/60/100 cm; compare levels before gain and limiter activity.
- [ ] At least 30 minutes of continuous audio in each mode; record overruns,
      underruns, PCM drops and encoded drops during steady operation.
- [ ] Recovery after RF interruption and remote audio suspend (automatic retry
      on a later heartbeat, up to approximately 10 seconds after suspension).
- [ ] Correct behavior with another supported headphone name and the right slot
      when L/R is physically wired to 3.3 V.
- [ ] Clear unsupported-format error for an incompatible sink, without playback.
- [ ] At low headphone volume, verify missing microphone data does not crash the
      connection; a disconnected I2S DATA wire can float, so electrical noise is
      not equivalent to a software read timeout.
- [ ] Acoustic latency measurement following [AUDIO.md](AUDIO.md); record median,
      minimum and 95th percentile rather than inferring delay from buffer capacity.
- [ ] Add an actual wiring/module photo showing the sound opening and GPIO labels.

## Before tagging a public release

Update the evidence above and change `PROJECT_VER` to the release version only
when the stated tests are complete. Publish known limitations with the release.
Keep build artifacts out of Git. If distributing binaries through Releases,
first resolve the GPLv3 corresponding-source requirements for the precompiled
controller/PHY libraries described in `THIRD_PARTY_NOTICES.md`. Then record
board/configuration, SDK commit and checksums, and include the required source,
installation information where applicable, and SDK/component license material.

# Contributing to Make2Hear

Keep changes incremental and explain the problem they solve. The project values
readable C, low latency, intelligible speech and stable continuous streaming.
Keep the microphone task, small PCM ring and nonblocking A2DP callback simple.

## Before proposing a change

1. Follow the README with the exact SDK in `.idf-version`.
2. Build both `B3_LOW_LATENCY_A2DP=ON` and `OFF` using fresh local configurations.
   CI does this without the developer's saved `sdkconfig`.
3. Run `python3 tools/check_build.py BUILD_DIRECTORY ON` (or `OFF`) on each
   fresh-default build to check public defaults and the selected source mode.
4. For audio, pairing or timing changes, record the relevant hardware tests in
   [docs/VALIDATION.md](docs/VALIDATION.md). Distinguish built from hardware-tested.
5. Explain changes to latency, buffer capacity, gain, format or supported hardware.

Do not commit build directories, local `sdkconfig`, logs, private paths, or paired
device addresses. Keep upstream notices. Contributions use the project's terms
described in `THIRD_PARTY_NOTICES.md`.

For bugs include the board/module, headphone model, SDK commit, firmware revision,
wiring/slot, gain, latency mode, reproduction steps and a short relevant log.
Replace Bluetooth addresses and pairing codes with placeholders before posting.
Recordings/timing measurements are useful if you have permission to share them.
Avoid heavy DSP dependencies or larger buffers without measurements explaining
the trade-off.

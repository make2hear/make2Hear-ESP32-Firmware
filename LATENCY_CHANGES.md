# Latency reduction report

Historical report from the latency update, before publication preparation.
See [README.md](README.md) for current installation, settings and supported SDK.

Scope: the INMP441 → A2DP → B3 project. Changes are incremental and keep
the microphone task, ring, callback, mono format, codec parameters, DSP and B3
connection handling. Gain at the time of this update was 8×; the public default is now 2×. No resampling was added.

## Changes and reasons

| Item | Previous | New default | Reason / trade-off |
|---|---|---|---|
| SBC source media cycle | 30 ms | **10 ms** | Encode/send smaller batches sooner; about three times as many media wakeups/ordinary packets |
| Normal callbacks per cycle | 10–11 | **3–4** | Each still requests 128 mono samples / 256 PCM bytes |
| PCM ring | 1,536 samples / 3,072 B / 34.83 ms | **640 samples / 1,280 B / 14.51 ms** | Four SBC frames plus 128 samples of margin; less tolerance for stalls |
| I2S block/read | 128 samples / 512 B / 2.90 ms | **64 samples / 256 B / 1.45 ms** | Earlier capture delivery; twice as many DMA completions/task wakeups |
| DMA descriptors | 3 | **6** | Preserve 384 samples / 1,536 B / 8.71 ms total scheduling tolerance; this is not a prefill delay |
| Source encoded queue (`TxAaQ`) | SDK's much larger mixed frame/packet guard | **Two complete packets** | Discard oldest packets under congestion instead of retaining stale audio; discards may be audible |
| Maximum catch-up frames/event | 21 | **7** | Preserve the SDK's approximate two-cycle catch-up allowance after changing the timer |
| Fractional PCM-byte pacing | Fraction truncated each event | **Remainder carried between events** | Prevent additional systematic rate loss when the media timer runs more often; no resampling or audio buffer |
| Diagnostics | Levels, underruns, DMA errors, occupancy snapshot | **Also timing, peak occupancy and encoded drops** | Observe the latency/stability trade-off on hardware |

The 20.32 ms reduction in PCM ring capacity is **not a measured 20.32 ms
end-to-end improvement**. Moving from 30 to 10 ms batching is expected to
reduce average batching wait by roughly 10 ms and worst nominal cycle wait by
roughly 20 ms. These waits overlap ring residence; do not add both savings.
Halving capture blocks saves up to 1.45 ms of block-fill wait, about 0.73 ms
on average for uniformly arriving samples.

Two ordinary encoded packets represent about 17–23 ms of audio with the new
cycle and a sufficiently large negotiated MTU. Catch-up packets can contain
up to seven frames each: two such packets represent 40.63 ms. A smaller peer
MTU can split a burst. Packet count is not a guaranteed wall-clock latency
bound, and downstream L2CAP/controller/B3 queues remain outside this cap.

Catch-up after a substantial scheduling stall can request more PCM than the
small ring retains. Existing silence-on-underrun behavior remains; the reduced
catch-up allowance is not a guarantee that all fault recovery is glitch-free.
No change was made to RTP timestamp generation or connection management.

## Why stop at 10 ms for this version?

The audited SDK documents its media interval as a multiple of the RTOS tick;
the project currently uses a 10 ms tick. The timer is high resolution, but
assuming that every related scheduling path is safe at a smaller interval
without measurements would go beyond the evidence. Likewise, the ring must
cover a complete normal four-frame burst and some scheduling jitter.

This is a deliberately aggressive first hardware-test configuration, not a
claim that 10 ms is a protocol minimum or the fastest setting B3 can support.
Further reductions should follow timing and underrun measurements. CPU speed,
compiler optimization, task placement/priority and DSP were kept unchanged to
make the effect of the buffering changes easier to compare.

## Files

- `main/main.c`: smaller capture/PCM blocks, matching original-buffer fallback,
  startup configuration log, timing counters and a packet-drop counter hook.
- `main/CMakeLists.txt`: explicit `esp_timer` dependency for timing measurements.
- `CMakeLists.txt`: includes the project-local A2DP build adjustment.
- `cmake/a2dp_low_latency.cmake`: changes one generated SDK media source for this
  project only, limits source packets and scales catch-up frame count.
- `README.md`: current configuration and diagnostic interpretation.
- `LATENCY_AUDIT.md`: marked as the historical baseline.
- `LATENCY_CHANGES.md`: this report and test instructions.

The CMake adjustment is restricted to ESP-IDF 6.1. It checks that each patch
anchor occurs exactly once and that the original media source is in the BT
target. It fails explicitly on an unsupported source layout. The generated
copy lives in `build_mic/b3_audio/btc_a2dp_source.c`; the original installed
SDK is not edited or copied wholesale into the project.

The SDK's original elapsed-time calculation truncates fractional PCM bytes
per event. Its rounding error can accumulate faster at 100 events/second.
The generated source retains that fractional remainder in one integer field
and resets it with the SDK's existing feeding state. This avoids that source
of rate drift; it does not synchronize the independent microphone/headphone
clocks or promise that all sample drops disappear.

## Build, flash and compare

Activate the pinned SDK and open this repository (replace `/path/to`):

```sh
. /path/to/esp-idf-make2hear/export.sh
cd /path/to/make2hear
idf.py -B build_mic -DB3_LOW_LATENCY_A2DP=ON build
ls /dev/cu.*
idf.py -B build_mic -p PORT flash monitor
```

Replace `PORT` with the ESP32 serial port. The startup log should show
`A2DP cycle=10 ms`, `I2S block=64 samples`, `DMA=6 blocks`, and
`PCM=640 samples (14.51 ms)`.

For an A/B comparison or fallback, restore the original media implementation
and capture/ring settings together:

```sh
idf.py -B build_mic -DB3_LOW_LATENCY_A2DP=OFF build
idf.py -B build_mic -p PORT flash monitor
```

`OFF` selects the installed SDK media source, 30 ms cycles, 128-sample reads,
three DMA descriptors, and the 1,536-sample ring. New diagnostics remain.
Use `-DB3_LOW_LATENCY_A2DP=ON` to re-enable the changes; CMake remembers the
last selected mode for that build directory.

## Hardware acceptance test

After connection settles, capture logs for several minutes and listen to
speech and short clicks. Compare both build modes with identical headphones,
gain, volume, distance and RF conditions.

| Field | What to look for |
|---|---|
| `fmt` | Must become 1 |
| `read_fail`, `dma_overrun` | Should remain zero during steady streaming; failures indicate capture/scheduling trouble |
| `underrun`, `missing` | Should remain zero or rare outside startup/transitions; continuous increases mean the smaller ring cannot sustain current timing |
| `drop` | PCM samples discarded; frequent drops suggest stalls or capture/consumption drift |
| `tx_drop` | Whole source SBC packets discarded; sustained increases indicate downstream congestion and may cause gaps |
| `q_high` | Peak occupancy, at most 640 samples / 14.51 ms in the new mode; reaching capacity alone is not proof of failure |
| `dsp_max` | Maximum processing time per 64-sample block, including preemption; normally well below 1,451 us |
| `cb_bytes_max`, `cb_n` | Normally 256 bytes and roughly 344–345 calls/s; calls still arrive in bursts |
| `cb_max` | Callback body time through the final measurement, including lock wait; should be much shorter than the media cycle |
| `cb_gap_max` | Maximum callback entry-to-entry gap; ordinarily around a 10 ms cycle, varying with encoding/scheduling |

These are execution/occupancy diagnostics. They do not measure individual
sample age, RF packet loss or B3's playback buffer. The measurements add no
audio windows or logging inside the callback/reader. All reports run from
the existing application event task once per second.

If steady underruns increase while I2S stays healthy, first compare `OFF`.
A possible next experiment is a 768-sample PCM ring (17.41 ms) with the 10 ms
cycle. If packet drops persist, investigate RF conditions and downstream
queues before reducing their limits further. Never change only the ring to
640 while retaining the stock 30 ms media cycle.

Measure acoustic delay using a common two-channel recording of sound at the
INMP441 opening and vibration/output at a B3 transducer. Compare median and
95th-percentile delay over repeated clicks, including a several-minute run.
If ESP32 timing is healthy but audible delay changes little, B3's receiver
buffer likely limits further gains from this A2DP path. See the measurement
procedure in `LATENCY_AUDIT.md`.

## Validation status

- ESP32 build succeeded with the low-latency mode enabled, including the final
  fractional pacing correction. Final image: `build_mic/make2hear.bin`.
- The original-buffer fallback build succeeded; the final build directory was
  returned to `B3_LOW_LATENCY_A2DP=ON`.
- Compile commands contain exactly one A2DP media source, the generated copy,
  and matching 10 ms application definitions. The final ELF contains a
  1,280-byte PCM ring and the packet-drop diagnostic hook.
- A host C test compiled the actual generated pacing calculation and verified
  cumulative PCM-byte counts against elapsed time over 360,000 events for
  each of three schedules: exact cadence, jitter, and stalls. Feeding state
  was reset between runs. This checks arithmetic, not Bluetooth scheduling.
- The installed SDK source checksum and project `sdkconfig` are unchanged.
  The sample-conversion/HPF/gain/limiter function bodies are unchanged.

No hardware flash, listening test, or end-to-end timing measurement has been
performed in this change session. The expected timing improvements above
are calculations, not measured results. Hardware acceptance remains necessary.

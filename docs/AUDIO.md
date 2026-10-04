# Audio processing and diagnostics

The implementation remains in `main/main.c`. `microphone_task()` reads one DMA
block, calls `process_audio_sample()` for each sample, and publishes 16-bit mono
PCM. `bt_app_a2d_data_cb()` copies available samples and fills missing bytes with
zero. It does not read I2S, allocate memory, filter, or wait for data.

## Conversion and processing

INMP441 Philips I2S uses 64 clocks per WS frame: two 32-bit slots. The driver
clocks both and stores the selected slot as one `int32_t` per sample at 44100 Hz.
The signed 24-bit value occupies bits 31:8. Arithmetic `raw >> 8` extracts it;
the other eight bits are outside the microphone word.

The first-order 100 Hz HPF removes DC and low-frequency rumble. Its state is just
the preceding input/output. Integer gain uses `int64_t` before final PCM
conversion. The instantaneous-attack limiter has a ceiling of 28000 in 16-bit
units and an 80 ms gain recovery. Recovery is not an 80 ms audio delay. Final
rounding divides by 256 to map 24-bit full scale to 16-bit full scale; saturation
protects the final conversion. Already-clipped microphone input cannot be repaired.

This is fixed gain with peak limiting, not AGC. There is no resampling, noise
gate, low-pass filter, spectral processing, or future-sample look-ahead. Confirmed
mono SBC consumes one PCM value per time sample, so do not duplicate L/R here.

## Diagnostics

Two lines are logged once per configured interval. Counters/peaks reset per
reported interval. If the control queue is busy, a diagnostic tick can be skipped;
the next report then spans a longer interval than its nominal label. `queued`
is an instantaneous count, not an interval peak. Ignore startup when evaluating
steady streaming.

| Field | Meaning and useful interpretation |
|---|---|
| `fmt` | 1 means the preferred PCM/SBC configuration was accepted; 0 keeps output silent/stopped |
| `gain` | Configured fixed multiplier |
| `n` | Captured samples in the interval; about 44100 for an uninterrupted second |
| `raw_pk` | Peak before HPF/gain, in 16-bit-equivalent units (full scale ≈32768) |
| `pre_pk`, `pre_avg` | Peak and mean absolute amplitude after HPF, before gain; mean absolute is not RMS |
| `in_near_fs` | Input samples ≥99% of microphone full scale; possible microphone overload |
| `limited` | Samples affected by limiter gain reduction, including its recovery tail |
| `clip` | Samples reaching final saturation; normally zero with the limiter |
| `pcm_pk` | Peak of the final 16-bit PCM |
| `read_fail` | I2S read errors, timeouts or short reads; successful reads can still coexist with DMA overruns |
| `dma_overrun` | DMA receive-queue overflow; capture was not drained in time |
| `underrun`, `missing` | A2DP requests short of PCM, and missing samples replaced by silence |
| `drop` | Old PCM samples discarded when the ring is full while streaming |
| `queued`, `q_high` | Current occupancy / highest streaming occupancy, in mono samples |
| `dsp_max` | Maximum block processing time, including preemption, in µs |
| `cb_n`, `cb_bytes_max`, `cb_max` | Callback count, largest requested length, maximum duration in µs |
| `cb_gap_max` | Longest gap between callbacks, including normal batching intervals |
| `tx_drop` | Encoded packets discarded by the source-queue patch; not RF losses |

Queue duration is `samples * 1000 / 44100`. The low-latency ring holds at most
14.51 ms of PCM. Full capacity, measured occupancy and end-to-end latency are
different quantities. Timing fields do not track a sample through the headphones.

## Tune speech audibility

1. Fix headphone volume and room conditions. Begin at 2× gain.
2. Record quiet-room `pre_avg` and `pre_pk`, then speak normally at 10, 30, 60
   and 100 cm, observing the same fields and `in_near_fs`.
3. If speech is above room noise but too quiet, try 4× and then 8× in menuconfig.
   1× = 0 dB, 2× ≈ +6 dB, 4× ≈ +12 dB, 8× ≈ +18 dB.
4. Test nearby louder speech. Persistent `limited` means the limiter is working;
   frequent limiting during ordinary speech suggests less gain. `clip` should
   remain zero. `in_near_fs` calls for more distance or different placement.
5. Resolve `dma_overrun`, `underrun` or `tx_drop` before treating crackles as
   microphone noise. Compare stock batching if timing is unstable.

Gain raises background noise too. Statistics cannot by themselves separate
microphone self-noise, room noise and electrical interference. Compare quiet-room
recordings, wiring, power, and an unobstructed microphone outside the enclosure.
Software cannot recover sound blocked by the enclosure.

## Measure actual latency

Record a reference microphone beside the INMP441 and a pickup on a headphone
transducer on two channels of the same recorder/interface. For bone conduction,
a contact pickup may work better than an airborne microphone. Use repeated
clicks and control acoustic leakage by repeating with headphone output muted.

Measure `sample_offset * 1000 / recording_sample_rate`, correcting unequal
acoustic path lengths. Report minimum, median and 95th percentile, alongside
headphone model, firmware revision, latency mode and diagnostic counters.
Application buffer calculations cannot substitute for this measurement.

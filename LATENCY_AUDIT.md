# INMP441 → A2DP → B3 latency audit

**Historical baseline:** this audit describes the configuration before the
latency reduction. See [LATENCY_CHANGES.md](LATENCY_CHANGES.md) for the current
10 ms A2DP cycle, 64-sample reads and 640-sample PCM ring. Source line numbers
below refer to the audited version.

**Historical settings include 8× gain; the public default is now 2×.**

Audited 2026-09-26. Scope: the earlier microphone project, `main/main.c`, its `sdkconfig`, and the installed ESP-IDF used by `build_mic` at `$IDF_PATH`. The separate `bone_conduction_ESP_only` HFP experiment is outside this audit.

This is a source-level audit. No firmware, configuration, or buffer sizes were changed, and no hardware timing measurements were taken. Calculated capacities are not measured queue occupancy or guaranteed worst-case execution times.

## Findings

- The active preferred format is **44,100 Hz, signed 16-bit mono**, SBC with 16 blocks and 8 subbands. It is no longer the original stereo example. All sample counts below mean mono time samples.
- Capture delivers **128 samples every 2.902 ms**. There are three DMA buffers, but capture does not wait for all three to fill.
- The application PCM ring holds **1,536 samples / 3,072 bytes / 34.830 ms**. It retains the newest audio when full.
- The installed A2DP source runs a **30 ms media timer** and normally pulls 10 or 11 SBC frames per timer event. This burst consumption explains the ring's size.
- DSP has no look-ahead or additional audio window. The limiter's 80 ms recovery setting is not 80 ms of audio delay.
- Additional encoded-packet queues exist inside ESP-IDF. Their occupancy is currently unmeasured. Limiting the application ring does not limit all downstream latency.
- B3's receive/playback delay is unknown. No defensible exact end-to-end total or B3-specific minimum can be inferred from these files alone.

## 1. Budget and accounting

For mono audio, `duration_ms = samples * 1000 / 44100`. Raw I2S samples occupy four bytes; application PCM samples occupy two.

| Stage | Actual storage or cadence | Latency contribution / interpretation |
|---|---|---|
| Sound travelling to microphone | Depends on distance | Approximately 2.9 ms per metre in air; exclude this when measuring from sound arrival at the microphone |
| INMP441 digital filter | Datasheet group delay: 17.2 sample periods | **0.390 ms** at 44.1 kHz |
| One I2S DMA block | 512 B = 128 raw samples | Samples wait approximately **0–2.90 ms** for block completion; mean about 1.45 ms for uniformly distributed arrivals |
| Entire DMA allocation | 3 × 512 B = 384 raw samples | **8.71 ms capacity**, not a mandatory 8.71 ms delay; delayed servicing can leave completed blocks pending |
| Reader local buffers | Raw 512 B + converted PCM 256 B; both represent the same 128 samples | Already covered by capture block size; add actual copy/DSP/scheduling execution time, currently unmeasured |
| PCM ring / wait for A2DP consumption | 3,072 B = 1,536 samples = 12 blocks | **34.83 ms capacity**; occupancy cycles as A2DP takes bursts. Approximately 0–35 ms residence under continuous healthy production, depending on phase/backlog |
| HPF, gain and limiter | Previous filter state and limiter gain; no audio window | **0 ms added buffering**; frequency-dependent HPF group delay and unmeasured CPU time |
| A2DP application callback | Normally 256 B = 128 samples per call | Copy/zero-fill time and brief spinlock contention only; no deliberate wait |
| A2DP media batching | 30 ms timer; normally 10–11 calls per event | Main known batching interval; much of this waiting is **already in the PCM ring** |
| SBC encoder | 128 samples = 2.90 ms of audio per frame | Frame already collected upstream; encoding execution and codec filter delay remain. Do not add another full realtime collection period per callback |
| Encoded queues inside ESP-IDF | Variable SBC packet sizes and queue occupancy | Unmeasured; potentially multiple packet intervals during congestion |
| Bluetooth ACL transport | Negotiated link scheduling, controller queues, retransmissions | Unmeasured; no fixed latency is established by this code |
| B3 receive, decode and output | Headphone jitter/decoder/output buffers | External and unmeasured; may dominate the total |

The microphone figure comes from the [INMP441 datasheet, electrical characteristics](https://product.tdk.cn/system/files/dam/doc/product/sw_piezo/mic/mems-mic/data_sheet/inmp441.pdf).

**Do not sum every row.** DMA descriptors, driver queue pointers and reader arrays refer to the same captured samples. Similarly, ring residence and the 30 ms consumer cadence overlap. Encoded bytes cannot be divided by the PCM byte rate to calculate their duration.

A useful working estimate for **microphone arrival → application callback** is about **20 ms average**, with ordinary sample ages extending toward **35–40 ms**, assuming continuous capture, little DMA backlog, and punctual Bluetooth servicing. This estimate follows from roughly half a 30 ms consumption cycle, DMA completion, and a small residual ring backlog. It is not a measurement or a hard bound. Phase, startup, drift, task stalls and overruns change it. In particular, full **ESP32** latency also includes SBC and downstream stack/controller queues, which this estimate does not include.

## 2. I2S and reader task

Sources: [configuration and init](main/main.c), [I2S initialization](main/main.c), [reader task](main/main.c).

The configuration uses I2S0 master RX, APLL, Philips format, two physical 32-clock slots, and only the left slot stored in mono mode. Thus there are 64 BCLKs per sample period, but only one 32-bit word per sample in DMA. Counting both physical slots as stored data would incorrectly double the capture-buffer duration.

`I2S_READ_SAMPLES=128` controls both `dma_frame_num` and the application's read size. `I2S_DMA_BUFFERS=3` controls `dma_desc_num`.

The installed `components/esp_driver_i2s/i2s_common.c` confirms:

- DMA completion enqueues a pointer to a completed buffer and can immediately wake the reader using an ISR yield.
- The RX completion queue contains `desc_num - 1` pointers: two pointers, eight bytes on ESP32. These reference up to 256 samples / 5.80 ms of the existing DMA allocation; they do not allocate another 5.80 ms audio buffer.
- `i2s_channel_read()` waits for completed DMA data, copies it to the caller, and continues until its requested byte count is satisfied or a timeout occurs.

Here the read requests 512 bytes, exactly one DMA block. Its `20` argument is a **20 ms timeout**, not a fixed delay added to every successful read. The reader can also return immediately when a completed block is already available. Multiple waits can occur inside the API on exceptional paths, so the timeout should not be treated as a guaranteed whole-task execution bound.

The reader runs at priority **18 on core 1**. Initialization happens on that core, placing the I2S interrupt there too. Bluetooth runs on core 0; the BTC task uses priority 19 in this SDK. `CONFIG_FREERTOS_HZ=100` gives a 10 ms tick, but interrupt wakeups do not have to wait for the next tick. There is no normal `vTaskDelay()` in the reader. The one-tick delay is only an immediate-error recovery path.

Higher-priority work, interrupts, critical sections and cache stalls can still delay capture. `CONFIG_I2S_ISR_IRAM_SAFE` is unset. This matters if flash operations disable cache during streaming; the audit has not established such operations as a current source of delay.

The task processes the entire returned block before publishing it. Its raw and converted arrays each represent the same block, so there is no additional block-fill wait between them. Copy and processing time must be measured. At 160 MHz with the current debug optimization setting, sustained processing must comfortably fit inside the 2.90 ms block period.

## 3. PCM ring and actual occupancy

The ring contains 12 capture blocks and can accumulate multiple blocks. The callback never waits for it to reach a prefill threshold. When full, the producer discards the oldest samples before writing the newest. Stream-reset/format-handling paths clear the ring.

Current diagnostics print an instantaneous `queued` sample count once per second. Convert it with:

```text
queued_duration_ms = queued * 1000 / 44100
```

Examples: 512 → 11.61 ms; 1,024 → 23.22 ms; 1,536 → 34.83 ms. These measure audio available at that instant, not the measured residence time of a specific sample. Sparse snapshots can miss the peak immediately before a burst and the minimum immediately after it.

**Normal measured fullness is unknown.** Earlier supplied logs showing `queued=0…1024`, DMA overruns and underruns predate the current configuration/diagnostics. They should not be used as measurements of this version. During a pause the current ring can become full even though no audio is being transmitted.

The application's Bluetooth control-event queue does not carry PCM. Its number of events and task stack sizes are not audio durations.

## 4. Processing latency

Sources: [gain/limiter](main/main.c), [sample processing](main/main.c).

Pipeline: arithmetic `raw >> 8` extracts signed 24-bit microphone data → first-order 100 Hz HPF → fixed **8× gain** in a wider integer type → instantaneous-attack limiter → rounded 16-bit PCM with final saturation.

There is no AGC, resampling, low-pass filter, FFT, moving audio window or look-ahead. Diagnostic averages accumulate numbers without retaining audio.

The HPF uses the previous input/output. Its phase introduces frequency-dependent group delay: approximately 0.8 ms around 100 Hz and 0.016 ms at 1 kHz for the implemented coefficient. This is not a uniform delay of the whole signal. Gain and saturation have no algorithmic delay. The limiter immediately responds to the current sample; `LIMITER_RELEASE_MS=80` controls subsequent gain recovery and adds no buffering.

CPU cost includes floating-point operations and integer arithmetic/divisions. No cycle measurements exist, so assigning an exact microsecond figure would be speculation. Measure the 128-sample processing loop and callback separately before changing DSP or CPU frequency.

## 5. Callback and SBC media cycle

Sources: [callback](main/main.c), [preferred codec configuration](main/main.c); installed SDK `components/bt/host/bluedroid/btc/profile/std/a2dp/btc_a2dp_source.c`, especially `btc_get_num_aa_frame()`, `btc_media_aa_read_feeding()`, and `btc_media_aa_prep_sbc_2_send()`.

For the accepted mono configuration:

```text
SBC samples per frame = 16 blocks × 8 subbands = 128
PCM bytes per callback = 128 × 1 channel × 2 bytes = 256
Nominal frames per 30 ms = 44100 × 0.030 / 128 = 10.33594
```

The stack therefore normally makes **10 or 11 callback calls close together every 30 ms**, not one call evenly spaced every 2.90 ms. That is 1,280 or 1,408 samples / 2,560 or 2,816 PCM bytes per burst, representing 29.02 or 31.93 ms of audio. Long-term callback frequency is approximately 344.5 calls/second. Delayed servicing can request catch-up bursts of up to 21 frames / 60.95 ms, subject to queue pressure.

The media timer uses the SDK alarm/`esp_timer` mechanism, then posts work to the BTC task. Scheduler/work-queue delays are additional and unmeasured. A defined 15 ms `A2DP_DATA_READ_POLL_MS` is not used on this path; it is not another 15 ms stage.

The callback zero-fills its output and copies available ring data directly into the encoder's PCM workspace. It has no I2S call, heap allocation, logging, or blocking queue read. Missing audio remains silence and the callback returns the requested length. There is a short cross-core spinlock around the copy; its duration is unmeasured.

The encoder workspace can hold 512 PCM bytes for stereo, but mono uses 256 bytes for the current frame. Codec history/work arrays are not another FIFO of that size. Matched sample rates bypass the SDK resampling path.

The encoder processes each frame immediately in the burst. It assembles SBC packets and submits even a partially filled packet at the end of that event; there is no mandatory wait until the next timer event to fill the packet. Codec analysis/synthesis filtering still has delay beyond software execution time; its exact combined encoder/B3-decoder delay is not established here.

## 6. Internal stack, transport and headphone buffering

The source allocates **4,112 bytes per outgoing compressed-packet buffer**. This is allocation capacity, not 4,112 bytes of PCM. Mono SBC frames contain `8 + 2 × bitpool` bytes with these block/subband settings: at most 78 bytes at bitpool 35. Actual bitpool and peer MTU require runtime observation. The local maximum A2DP MTU is 1,008 bytes. At that MTU, an ordinary 10–11-frame burst fits in one packet; a smaller negotiated MTU can split it.

`TxAaQ` holds encoded packets. `MAX_OUTPUT_A2DP_SRC_FRAME_QUEUE_SZ=27` participates in congestion checks that mix the packet queue length with the number of frames to encode. It must **not** be interpreted as a simple 27-SBC-frame audio FIFO. Ordinary packets represent roughly 30 ms, so several waiting packets can mean substantial backlog: five such packets represent about 150 ms of media. This is a capacity illustration, not evidence that five packets currently wait or a guaranteed wall-clock delay. The SDK comment describing a typical queue length of about one packet is likewise not a measurement of B3.

Further downstream, `bta_av_data_path()` checks congestion and L2CAP queued packets. Its send threshold is five; `bta_av_cfg.c` declares an AVDTP audio queue maximum of six. A holding list can retain packets while the lower layer is congested. These are compressed packet queues with unknown current payload sizes/occupancy; they cannot be assigned exact PCM byte capacities or simply summed into a fixed latency. Controller scheduling and RF retransmissions add uncertainty.

These defaults are internal SDK code, not adjustable application ring settings. Queue trimming could trade stale audio for audible loss, and reducing the media period involves coupled frame-budget/scheduling assumptions. Neither was changed.

B3 can buffer received packets, decode SBC, perform internal audio processing and queue output samples. Those delays are outside this firmware's direct control. Existing `ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT` handlers already print `delay value: N * 1/10 ms`: divide N by 10 for milliseconds. If B3 reports this, record it, but treat it as a sink declaration rather than a physical measurement. It is not the whole microphone-to-output path. [Espressif's A2DP API](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/bluetooth/esp_a2dp.html) describes sink delay as including receive buffering, decoding and rendering. Do not add those components again to a reported total. The documented ESP-IDF sink default of 120 ms is not a universal A2DP minimum and is not a B3 measurement.

For context only, [TI's Bluetooth engineering guidance](https://e2e.ti.com/support/wireless-connectivity/bluetooth-group/bluetooth/f/bluetooth-forum/625462/cc2564-clock-synchronisation-for-a2dp-streaming-audio) gives roughly **150–200 ms** for a typical A2DP source/sink buffering and encode/decode chain, with retransmissions/drift potentially adding delay. This is an example from another implementation, not an ESP32/B3 measurement or a value to add wholesale to every row above. It supports expecting an order-of-100-ms system, but cannot allocate an exact post-callback or headphone-only figure here.

## 7. What can be reduced, and the stability limit

| Source | Classification | Control / trade-off |
|---|---|---|
| I2S block and matching read size | EASY TO REDUCE | `I2S_READ_SAMPLES`; 64 would be 1.45 ms/block, but doubles interrupt/task wakeup frequency. Expected saving is small |
| DMA backlog capacity | EASY TO REDUCE | `I2S_DMA_BUFFERS`; fewer descriptors reduce scheduling tolerance, not the normal single-block capture wait |
| PCM ring capacity | EASY TO REDUCE mechanically | `AUDIO_BUFFER_SAMPLES`; cannot safely ignore the 30 ms consumer burst |
| Reader/DSP execution and scheduling | POSSIBLE TO REDUCE | `MIC_TASK_PRIORITY`, `MIC_TASK_CORE`, CPU/optimization settings; measure first. Capture is already separated from Bluetooth |
| Cache-related capture stalls | POSSIBLE TO REDUCE | `CONFIG_I2S_ISR_IRAM_SAFE`, if runtime evidence shows this problem; requires callback/data placement review |
| A2DP 30 ms cadence / encoded backlog | POSSIBLE TO REDUCE | Internal `BTC_MEDIA_TIME_TICK_MS`, frame budgets and queue logic; coupled SDK changes, not ordinary application settings |
| INMP441 internal filter delay | PROBABLY NOT UNDER OUR CONTROL at fixed rate | Only 0.39 ms; sample-rate changes would affect the rest of the pipeline |
| SBC algorithm and negotiated capabilities | POSSIBLE TO REDUCE within peer support | Framing/codec changes require negotiation and validation; reducing bitrate alone does not establish lower playback latency |
| Bluetooth controller scheduling/retries | PROBABLY NOT UNDER OUR CONTROL directly | RF conditions can improve stability, but there is no app constant giving a fixed transmission delay |
| B3 jitter/decode/output buffering | PROBABLY NOT UNDER OUR CONTROL | Measure it; a supported device latency mode could matter, but none has been established for B3 |

The smallest mathematical ring that holds a normal 11-frame burst is **1,408 samples / 31.93 ms**. It leaves little timing margin. The current **1,536 samples / 34.83 ms** adds only one 128-sample block of margin and is a sensible conservative baseline with the stock scheduler. It is not a proven universal minimum. A 1,024-sample ring holds only 23.22 ms and cannot reliably provide a 31.93 ms burst without relying on concurrent producer refills.

Retain the current 128-sample reads, three DMA descriptors and 1,536-sample ring for the measurement pass. Earlier overrun/underrun evidence makes a blind reduction especially unjustified. Changing only DMA descriptor count does not remove the 30 ms Bluetooth cadence. Reducing only the DMA block size while leaving a larger blocking read size can also fail to reduce delivery time.

Smaller buffers tolerate less scheduler/RF jitter; larger ones can absorb interruptions but retain older audio. Stability should be assessed with underrun/overrun/drop counts and latency percentiles together. Queue capacity alone does not establish normal latency or audio quality.

## 8. Measurement plan

### End to end

1. Record a reference microphone beside the INMP441 acoustic opening and a pickup at one B3 transducer on two channels of the **same recorder/audio interface**.
2. For bone-conduction output, a contact pickup is preferable if an ordinary microphone cannot reliably capture the transducer. Control direct acoustic leakage: repeat with headphone playback muted to identify the original sound leaking into the output recording.
3. Generate repeated short clicks or claps at a comfortable level. Record unprocessed audio, with recorder AGC/noise suppression disabled where possible.
4. Measure onset separation or cross-correlate the two channels: `latency_ms = sample_offset * 1000 / recorder_sample_rate`. Correct for unequal acoustic path lengths.
5. Repeat across startup, steady streaming and several minutes of use. Report median, minimum and 95th percentile over many clicks, together with firmware diagnostics. This reveals fixed buffering, periodic variation and drift.

### Within ESP32, in a later diagnostic pass

- Timestamp DMA EOF using `esp_timer_get_time()` in an appropriately placed receive callback; do not log or allocate there. Associate timestamps and sequence numbers with individual DMA blocks.
- Timestamp read completion, completion of the 128-sample DSP loop, and callback consumption. Carry compact per-block metadata alongside the ring; update it consistently on partial consumption, overwrite and reset.
- For sample i in a 128-sample block, approximate its digital acquisition time as `EOF_time - (127 - i) / 44100`. Account for interrupt latency, and invalidate discontinuities/overrun intervals. A timestamp taken only after `i2s_channel_read()` returns misses time already spent in DMA/driver queues.
- Accumulate callback duration, callback length, burst intervals, queue high/low water marks, and sample ages. Print aggregates once per second from a non-audio task. Do not use one global “last microphone timestamp” for samples of different ages.
- If application ages are small but acoustic delay remains high, instrument encoded queue depth and packet age around `TxAaQ`/AVDTP next. This separates post-callback ESP32 delay from receiver delay more effectively than guessing queue sizes.

## Conclusion

The largest known application timing interval is the 30 ms A2DP media cycle. The I2S block is only 2.90 ms, and DSP introduces no look-ahead. About 20 ms average to callback is a reasonable initial model; total on-chip delay and post-callback delay still need measurements. Headphone/stack buffering is the likely larger end-to-end contributor, but the current evidence cannot assign it a measured share.

No numerical B3-specific minimum can be defended yet. The stock burst schedule already requires continuous playback to accommodate roughly a 30 ms delivery cycle before adding codec, transport and receiver overhead. Plan for an order-of-100-ms A2DP system until measurement demonstrates otherwise; 150–200 ms is a generic reference, not a protocol floor. A reliably sub-20-ms target is not realistic with this unchanged path.

If substantially lower delay is required, investigate another transport/output path. The existing HFP/SCO experiment is relevant because it uses a voice-oriented transport, but measure its complete round trip and account for its voice bandwidth and headphone microphone processing. Wired output provides a useful low-latency baseline. No architecture changes were made in this audit.

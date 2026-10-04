# Make2Hear

Experimental live-listening firmware for a **classic ESP32**, an **INMP441 I2S
microphone**, and Bluetooth A2DP headphones. Developed from Espressif's
[A2DP source example](https://github.com/espressif/esp-idf/tree/fff9895c82d744c7237be8847347bdd1b07c6643/examples/bluetooth/bluedroid/classic_bt/a2dp_source).

```text
INMP441 → I2S RX → 24-bit samples → 100 Hz high-pass filter
        → adjustable fixed gain → limiter → 16-bit mono PCM
        → A2DP / SBC → Bluetooth headphones
```

The firmware continuously streams microphone audio and retries after a lost
connection. A dedicated microphone task feeds a small PCM ring; the A2DP audio
callback never waits for microphone data. There is no AGC or resampling.

**Status: experimental, 0.1.0-dev.** Earlier firmware worked with B3 headphones.
The publication changes still need the [hardware checks](docs/VALIDATION.md).
End-to-end latency depends on the headphones and has not been measured for this
release. The headset's own microphone/HFP experiment is a separate project.

## Hardware and wiring

| Part | Requirement |
|---|---|
| ESP32 board | Classic ESP32 with Bluetooth Classic; this project builds only for `esp32` |
| Microphone | INMP441 breakout, powered from 3.3 V |
| Headphones | Bluetooth Classic A2DP sink; B3 is the original development headset |
| Connection | USB data cable, stable board power, short microphone wires |

| INMP441 | ESP32 default connection |
|---|---|
| VDD | 3.3 V |
| GND | GND |
| SCK | GPIO 26 (BCLK) |
| WS | GPIO 25 |
| SD | GPIO 33 (input) |
| L/R | GND (left slot) |

GPIO numbers are chip GPIOs, not connector pin positions. If L/R is connected
to 3.3 V, select the **right slot** in menuconfig. Do not leave L/R floating.
Choose GPIOs exposed by your board; avoid pins used for flash or boot strapping.
The driver checks pin validity, but cannot determine your board's wiring.

Keep the microphone's acoustic opening clear. The INMP441 is a bottom-port
microphone; on a breakout, sound usually reaches it through a PCB opening.
Inspect your module before mounting it behind an enclosure opening. Gain cannot
recover sound blocked by plastic, glue, foam, or a poorly aligned sound hole.
See [TDK's INMP441 information](https://invensense.tdk.com/wp-content/uploads/2015/02/INMP441.pdf).

## Install the supported ESP-IDF

Both latency modes require ESP-IDF **6.1 at the exact commit in [.idf-version](.idf-version)**:
`fff9895c82d744c7237be8847347bdd1b07c6643`.
The build checks this revision. Use a dedicated SDK checkout to keep other
projects' installations independent.

Install the operating-system prerequisites from Espressif's
[getting-started guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/get-started/index.html).
For macOS/Linux, in a directory where you keep development tools:

```sh
git clone https://github.com/espressif/esp-idf.git esp-idf-make2hear
cd esp-idf-make2hear
git checkout fff9895c82d744c7237be8847347bdd1b07c6643
git submodule update --init --recursive
./install.sh esp32
. ./export.sh
```

For each new terminal, activate that SDK with:

```sh
. /path/to/esp-idf-make2hear/export.sh
```

Replace `/path/to` with your installation directory. On Windows use Espressif's
supported ESP-IDF terminal and the same pinned SDK; flashing uses a port such as
`COM5`. Build checks run locally on macOS and CI is configured for Linux;
Windows has not been verified for this release.

## Configure, build, and listen

Download or clone this repository, then open a terminal in its root directory:

```sh
cd /path/to/make2hear
idf.py set-target esp32
idf.py menuconfig
idf.py build
```

Use `set-target` on initial setup; it regenerates local configuration.
In **Make2Hear configuration**, set the headphones' exact Bluetooth name
(default **B3**), verify the GPIOs and microphone slot, and choose gain.
The generated `sdkconfig` is local and ignored by Git; `sdkconfig.defaults` and
Kconfig provide the shared defaults.

Find the board's serial port on macOS:

```sh
ls /dev/cu.*
```

On Linux look for `/dev/ttyUSB*` or `/dev/ttyACM*`; on Windows use Device Manager.
Replace `PORT` in:

```sh
idf.py -p PORT flash monitor
```

Put the headphones in pairing mode and disconnect any phone that automatically
reconnects to them. Allow discovery and the next 10-second connection heartbeat
to finish. Expected messages include:

```text
I2S microphone initialized at 44100 Hz; fixed gain 2x ...
Starting device discovery for 'B3'...
a2dp connected
PCM confirmed: 44100 Hz, signed 16-bit mono
a2dp media start successfully.
```

Adjust headphone volume using the headphones. The firmware observes volume
changes without raising it automatically. Exit the monitor with **Ctrl+]**.
Streaming is continuous; after a remote audio suspend it attempts to resume on
a subsequent heartbeat.

## Settings and audio

| Setting | Fresh-build default | Where to change it |
|---|---|---|
| Target headphone name | `B3` | Make2Hear menu; existing `EXAMPLE_PEER_DEVICE_NAME` symbol retained |
| ESP32 Bluetooth name | `Make2Hear` | Make2Hear menu |
| BCLK / WS / DATA | 26 / 25 / 33 | Make2Hear menu |
| Microphone slot | Left | Make2Hear menu |
| Fixed gain | 2× (+6 dB) | Make2Hear menu: 1×, 2×, 4×, 8× |
| Diagnostics interval | 1000 ms | Make2Hear menu |
| Legacy pairing PIN | `1234` | Make2Hear menu; match the headphone manual |
| HPF / limiter | 100 Hz / 28000 PCM units, 80 ms release | Constants near the top of `main/main.c` |
| Sample rate / format | 44100 Hz, signed 16-bit mono | Coupled to SBC configuration; keep matched |

The 24-bit signal occupies bits 31:8 of the received I2S word. Processing keeps
that precision through filtering and gain, then rounds to 16-bit PCM. The limiter
reduces gain immediately for large peaks and recovers gradually; final saturation
prevents integer wraparound. Neither adds a look-ahead buffer. See
[audio diagnostics and tuning](docs/AUDIO.md).

## Latency modes

The default **experimental low-latency mode** patches a generated copy of one SDK
source file. The installed SDK is unchanged. Source hash and replacement checks
reject unexpected SDK source. The source's encoded queue drops old packets
under congestion, which can cause audible gaps.

| Setting | Low latency (`ON`) | Stock batching (`OFF`) |
|---|---:|---:|
| A2DP media cycle | 10 ms | 30 ms |
| I2S samples per read | 64 (1.45 ms) | 128 (2.90 ms) |
| DMA descriptors | 6 (8.71 ms capacity) | 3 (8.71 ms capacity) |
| PCM ring | 640 samples / 1280 B / 14.51 ms | 1536 samples / 3072 B / 34.83 ms |
| Source encoded queue | At most 2 packets at enqueue | SDK behavior |

These are **buffer capacities and scheduling intervals, not end-to-end latency**.
Bluetooth transport and headphone playback buffering remain. Smaller buffers
also tolerate less scheduling/RF jitter.

Build and flash a stock-batching comparison on the same SDK:

```sh
idf.py -B build-stock -DB3_LOW_LATENCY_A2DP=OFF build
idf.py -B build-stock -p PORT flash monitor
```

Return to the default mode with `idf.py -B build -DB3_LOW_LATENCY_A2DP=ON build`
and flash from that directory. The switch changes microphone/ring capacities
together with the SDK cycle. Do not independently shrink the ring.

## Compatibility and troubleshooting

- **Not discovered:** verify the exact name, pairing mode, and that headphones
  are disconnected from other sources. Discovery requires a rendering device
  class and a name in EIR or the device-name property. Devices exposing neither
  are not currently supported. Avoid two discoverable devices with the same name.
- **Pairing failure:** SSP uses headless Just Works pairing. Keyboard/passkey entry
  is unsupported and explicitly rejected. Legacy pairing uses the configured
  PIN; a request for 16 characters requires a 16-character configured PIN.
- **Connected, no audio:** check for `PCM confirmed` / `fmt=1`. This version requires
  SBC 44.1 kHz mono, 16 blocks, 8 subbands, loudness allocation, and a bitpool range
  covering 2–35. Unsupported formats stay stopped with an error. Other headphones
  remain unverified; there is no automatic stereo or 48 kHz fallback.
- **`fmt=1`, but silence:** check VDD/GND/SD, L/R and slot selection, the sound
  opening, headphone volume, and microphone diagnostics.
- **Quiet/noisy/distorted:** use the [diagnostic guide](docs/AUDIO.md) before
  increasing gain. A rising underrun or DMA-overrun count is a timing problem.
- **SDK/patch error:** use a clean SDK at the pinned revision. `OFF` is a timing
  comparison, not support for arbitrary ESP-IDF versions.
- **Changing headphones:** update the target name and rebuild/flash. After finding
  a device, reconnection targets its address until reboot.

To reset stored Bluetooth pairings, clear the headset's pairing list using its
manual. If necessary, `idf.py -p PORT erase-flash` clears **all ESP32 flash data,
including firmware and NVS**; then run `idf.py -p PORT flash monitor` again.

## Development and license

- [Contribution guide](CONTRIBUTING.md) and [validation checklist](docs/VALIDATION.md).
- [Change log](CHANGELOG.md).
- Historical [latency audit](LATENCY_AUDIT.md) and [latency changes](LATENCY_CHANGES.md).
- Original Make2Hear contributions: [Apache-2.0](LICENSE).
  Espressif material retains its original terms; see [third-party notices](THIRD_PARTY_NOTICES.md).

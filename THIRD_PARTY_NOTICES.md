# Licensing and upstream material

Original Make2Hear contributions are copyright 2026 Make2Hear contributors and
licensed under [Apache-2.0](LICENSE). Upstream copyrights and terms are preserved.

## Espressif A2DP source example

`main/main.c`, `main/bt_app_core.c`, and `main/bt_app_core.h` are modified versions
of Espressif's example. Original portions carry:

```text
SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
SPDX-License-Identifier: Unlicense OR CC0-1.0
```

The upstream choices are included in [Unlicense](LICENSES/Unlicense.txt) and
[CC0-1.0](LICENSES/CC0-1.0.txt). Make2Hear changes add microphone capture, audio
processing/buffering, diagnostics, continuous streaming and publication fixes.
The combined files identify original portions and Apache-2.0 modifications.
Build/Kconfig scaffolding also originated with this example.

[Upstream example at the pinned revision](https://github.com/espressif/esp-idf/tree/fff9895c82d744c7237be8847347bdd1b07c6643/examples/bluetooth/bluedroid/classic_bt/a2dp_source).

## ESP-IDF SBC source adjustment

`cmake/a2dp_low_latency.cmake` contains replacement anchors and derived fragments
from `components/bt/host/bluedroid/btc/profile/std/a2dp/btc_a2dp_source.c`:

```text
SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
SPDX-License-Identifier: Apache-2.0
```

At build time it generates a modified copy retaining the upstream header, with
shorter batching, fractional-byte pacing and a bounded encoded-packet queue.
The generated copy is a build artifact, excluded from this repository.

## SDK and binary distribution

ESP-IDF is downloaded separately at the revision in `.idf-version`. It contains
components with their own licenses and notices. The root license does not replace
those terms. If distributing binaries, include the applicable SDK/component
licenses and notices for that build, alongside this project's notices. Consult
the pinned SDK's `LICENSE` and component files.

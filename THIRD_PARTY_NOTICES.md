# Licensing and upstream material

Original Make2Hear contributions are copyright 2026 Make2Hear contributors and
licensed under [GNU General Public License version 3 only](LICENSE)
(`GPL-3.0-only`). Make2Hear as a combined source project is distributed under
GPLv3, with the upstream copyrights and compatible license terms below preserved.

This change takes effect in the source tree on 2026-10-05. It does not revoke
permissions for copies previously distributed under Apache-2.0. No additional
linking exception is granted by this change.

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
The combined files identify original portions and GPL-3.0-only modifications.
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
Make2Hear's modifications to that source and the patch script are GPL-3.0-only;
the original SDK fragments remain Apache-2.0. A copy of the Apache license is
included at [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt).

Apache-2.0 code can be included in GPLv3 projects while retaining its notices;
see the [Apache Software Foundation's compatibility explanation](https://www.apache.org/licenses/GPL-compatibility.html).

## SDK and binary distribution

ESP-IDF is downloaded separately at the revision in `.idf-version`. It contains
components with their own licenses and notices. The root license does not replace
those terms or relicense Espressif's independent libraries.

The existing ESP32 link maps include precompiled `libbtdm_app.a` (Bluetooth
controller) and `libphy.a` (radio PHY). Their directories in the pinned SDK
provide Apache-2.0 license texts. However, permission to redistribute a library
and fulfillment of GPLv3's corresponding-source requirements for a combined
executable are separate questions. This source-license change does not establish
that corresponding source for those precompiled libraries is available, or that
they qualify for the GPLv3 System Libraries exception.

Before distributing firmware binaries or preflashed devices, establish how the
requirements of GPLv3 sections 1 and 6 will be met for that build, including any
applicable installation information. If necessary, obtain an appropriate
additional permission from the relevant copyright holders or use a compatible
alternative. Include the applicable SDK/component licenses and notices as well
as the required corresponding source. Publishing the Make2Hear source tree alone
does not establish compliance for distribution of the combined firmware binary.

References: [GPLv3 text](https://www.gnu.org/licenses/gpl-3.0.html),
[ESP32 Bluetooth library license](https://github.com/espressif/esp32-bt-lib/blob/master/LICENSE),
[ESP PHY library license](https://github.com/espressif/esp-phy-lib/blob/master/LICENSE).

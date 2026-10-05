#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Make2Hear contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Check a fresh-default build and that its SBC source matches its audio mode."""

import argparse
import json
from pathlib import Path
import shlex

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("build", type=Path)
parser.add_argument("mode", choices=("ON", "OFF"))
args = parser.parse_args()
build = args.build.resolve()
config = json.loads((build / "config/sdkconfig.json").read_text())

# These are the public first-use settings, independently of a local sdkconfig.
expected = {
    "IDF_TARGET": "esp32",
    "BT_ENABLED": True,
    "BT_CLASSIC_ENABLED": True,
    "BT_A2DP_ENABLE": True,
    "BT_A2DP_USE_EXTERNAL_CODEC": False,
    "BTDM_CTRL_MODE_BR_EDR_ONLY": True,
    "BT_BLUEDROID_PINNED_TO_CORE": 0,
    "BTDM_CTRL_PINNED_TO_CORE": 0,
    "FREERTOS_UNICORE": False,
    "FREERTOS_HZ": 100,
    "ESP_DEFAULT_CPU_FREQ_MHZ": 160,
    "EXAMPLE_PEER_DEVICE_NAME": "B3",
    "EXAMPLE_SSP_ENABLED": True,
    "MAKE2HEAR_DEVICE_NAME": "Make2Hear",
    "MAKE2HEAR_MIC_BCLK": 26,
    "MAKE2HEAR_MIC_WS": 25,
    "MAKE2HEAR_MIC_DATA": 33,
    "MAKE2HEAR_MIC_RIGHT_SLOT": False,
    "MAKE2HEAR_MIC_GAIN": 2,
    "MAKE2HEAR_STATS_PERIOD_MS": 1000,
}
for key, value in expected.items():
    if config.get(key) != value:
        raise SystemExit(f"Unexpected default {key}: {config.get(key)!r}, expected {value!r}")

commands = json.loads((build / "compile_commands.json").read_text())
main = [entry for entry in commands if Path(entry["file"]).name == "main.c"
        and Path(entry["file"]).parent.name == "main"]
sbc = [entry for entry in commands if Path(entry["file"]).name == "btc_a2dp_source.c"]
if len(main) != 1 or len(sbc) != 1:
    raise SystemExit("Expected exactly one application main.c and one SBC source")
flags = shlex.split(main[0]["command"])
period, enabled = (10, 1) if args.mode == "ON" else (30, 0)
for flag in (f"-DB3_A2DP_MEDIA_TICK_MS={period}", f"-DB3_LOW_LATENCY_A2DP={enabled}"):
    if flag not in flags:
        raise SystemExit(f"Missing coupled application mode flag: {flag}")

source = Path(sbc[0]["file"]).resolve()
description = json.loads((build / "project_description.json").read_text())
expected_source = (build / "b3_audio/btc_a2dp_source.c" if enabled else
                   Path(description["idf_path"]) /
                   "components/bt/host/bluedroid/btc/profile/std/a2dp/btc_a2dp_source.c")
if source != expected_source.resolve():
    raise SystemExit(f"Wrong SBC source for {args.mode}: {source}")
if not (build / "make2hear.bin").is_file():
    raise SystemExit("Firmware binary is missing; complete the build first")

print(f"PASS: fresh defaults, {args.mode} source/cadence coupling, make2hear.bin")

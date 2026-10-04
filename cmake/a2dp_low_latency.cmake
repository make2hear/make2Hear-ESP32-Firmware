# SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
# SPDX-FileCopyrightText: 2026 Make2Hear contributors
# SPDX-License-Identifier: Apache-2.0

# The public A2DP API cannot change the built-in SBC source's media timer.
# Patch one generated source in this build, keeping the installed SDK untouched.
option(B3_LOW_LATENCY_A2DP "Use 10 ms A2DP batching and bounded source queues" ON)
idf_component_get_property(b3_main_lib main COMPONENT_LIB)

if(NOT B3_LOW_LATENCY_A2DP)
    target_compile_definitions(${b3_main_lib} PRIVATE
        B3_LOW_LATENCY_A2DP=0 B3_A2DP_MEDIA_TICK_MS=30)
    message(STATUS "B3 audio: stock 30 ms A2DP source and original capture/PCM buffers")
    return()
endif()

if(NOT IDF_VERSION_MAJOR EQUAL 6 OR NOT IDF_VERSION_MINOR EQUAL 1)
    message(FATAL_ERROR "Make2Hear requires the pinned ESP-IDF 6.1 revision in .idf-version.")
endif()
if(CONFIG_BT_A2DP_USE_EXTERNAL_CODEC)
    message(FATAL_ERROR "B3 latency configuration requires the built-in SBC encoder.")
endif()

# 10 ms respects the existing 100 Hz tick constraint. Seven frames retain the
# stock ~2-tick catch-up allowance (21 frames at 30 ms -> 7 at 10 ms).
set(b3_media_tick_ms 10)
set(b3_max_frames_per_tick 7)
set(b3_tx_queue_packets 2)

idf_component_get_property(b3_bt_lib bt COMPONENT_LIB)
idf_component_get_property(b3_bt_dir bt COMPONENT_DIR)
set(b3_sdk_source "${b3_bt_dir}/host/bluedroid/btc/profile/std/a2dp/btc_a2dp_source.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${b3_sdk_source}")
file(READ "${b3_sdk_source}" b3_source)
file(SHA256 "${b3_sdk_source}" b3_source_sha256)
if(NOT b3_source_sha256 STREQUAL "8e4280d8dfffefd84e49ee62368ba99b1031a8da131592ed4e396127ba116495")
    message(FATAL_ERROR "The installed SBC source differs from the audited source. Use a clean SDK at the pinned commit, or disable B3_LOW_LATENCY_A2DP to diagnose local SDK changes.")
endif()

# Refuse an unexpected SDK source instead of silently building a small PCM
# ring against an unmodified 30 ms encoder. Each anchor must occur exactly once.
function(b3_replace_once needle replacement)
    string(FIND "${b3_source}" "${needle}" first)
    if(first EQUAL -1)
        message(FATAL_ERROR "B3 A2DP patch anchor missing: ${needle}")
    endif()
    string(LENGTH "${needle}" length)
    math(EXPR after "${first} + ${length}")
    string(SUBSTRING "${b3_source}" ${after} -1 tail)
    string(FIND "${tail}" "${needle}" second)
    if(NOT second EQUAL -1)
        message(FATAL_ERROR "B3 A2DP patch anchor is ambiguous: ${needle}")
    endif()
    string(REPLACE "${needle}" "${replacement}" changed "${b3_source}")
    set(b3_source "${changed}" PARENT_SCOPE)
endfunction()

b3_replace_once("#define BTC_MEDIA_TIME_TICK_MS                 (30)"
    "#define BTC_MEDIA_TIME_TICK_MS                 (${b3_media_tick_ms})")
b3_replace_once("#define MAX_PCM_FRAME_NUM_PER_TICK             21 // 14 for 20ms"
    "#define MAX_PCM_FRAME_NUM_PER_TICK             ${b3_max_frames_per_tick} // ~20 ms catch-up limit")
b3_replace_once("#define BTC_A2DP_SRC_DATA_QUEUE_IDX            (1)"
    "#define BTC_A2DP_SRC_DATA_QUEUE_IDX            (1)\n\n/* Diagnostic hook implemented in this project's main.c; no logging here. */\nextern void app_audio_tx_packet_dropped(void);")

# More frequent ticks otherwise truncate fractional PCM bytes more often.
# Carry the remainder so elapsed time, not timer jitter, determines the rate.
# The SDK already zeroes this entire feeding-state struct on start/stop.
b3_replace_once("    UINT32 bytes_per_tick;  /* pcm bytes read each media task tick */"
    "    UINT32 bytes_per_tick;  /* pcm bytes read each media task tick */\n    UINT32 byte_rate_remainder; /* Fractional byte numerator, not audio data. */")
set(b3_old_accumulator [=[        a2dp_source_local_param.btc_aa_src_cb.media_feeding_state.pcm.counter +=
            a2dp_source_local_param.btc_aa_src_cb.media_feeding_state.pcm.bytes_per_tick *
            us_this_tick / (BTC_MEDIA_TIME_TICK_MS * 1000);]=])
set(b3_precise_accumulator [=[        /* Retain fractional bytes across ticks: avoid systematic rate loss. */
        UINT64 scaled_bytes =
            (UINT64)a2dp_source_local_param.btc_aa_src_cb.media_feeding_state.pcm.bytes_per_tick * us_this_tick +
            a2dp_source_local_param.btc_aa_src_cb.media_feeding_state.pcm.byte_rate_remainder;
        a2dp_source_local_param.btc_aa_src_cb.media_feeding_state.pcm.counter +=
            scaled_bytes / (BTC_MEDIA_TIME_TICK_MS * 1000);
        a2dp_source_local_param.btc_aa_src_cb.media_feeding_state.pcm.byte_rate_remainder =
            scaled_bytes % (BTC_MEDIA_TIME_TICK_MS * 1000);]=])
b3_replace_once("${b3_old_accumulator}" "${b3_precise_accumulator}")

# Count packets here, separately from the SDK's older mixed frame/packet limit.
# Drop only whole SBC packets, retaining their existing RTP timestamps.
set(b3_enqueue [=[            /* Enqueue the encoded SBC frame in AA Tx Queue */
            fixed_queue_enqueue(a2dp_source_local_param.btc_aa_src_cb.TxAaQ, p_buf, FIXED_QUEUE_MAX_TIMEOUT);]=])
set(b3_bounded_enqueue "            /* Live audio: prefer recent packets over a growing stale backlog.
             * This bounds TxAaQ only; L2CAP/controller/headphone queues remain. */
            while (fixed_queue_length(a2dp_source_local_param.btc_aa_src_cb.TxAaQ) >= ${b3_tx_queue_packets}) {
                void *old_packet = fixed_queue_dequeue(a2dp_source_local_param.btc_aa_src_cb.TxAaQ, 0);
                if (old_packet == NULL) {
                    break; /* The consumer already drained the queue. */
                }
                osi_free(old_packet);
                app_audio_tx_packet_dropped();
            }
${b3_enqueue}")
b3_replace_once("${b3_enqueue}" "${b3_bounded_enqueue}")

set(b3_generated_dir "${CMAKE_BINARY_DIR}/b3_audio")
set(b3_generated_source "${b3_generated_dir}/btc_a2dp_source.c")
file(MAKE_DIRECTORY "${b3_generated_dir}")
# configure_file avoids rebuilding the source on an unchanged reconfigure.
file(WRITE "${b3_generated_dir}/btc_a2dp_source.c.in"
    "/* Modified by Make2Hear: 10 ms batching, fractional PCM pacing, bounded Tx queue.\n * SPDX-FileCopyrightText: 2026 Make2Hear contributors\n * SPDX-License-Identifier: Apache-2.0\n */\n${b3_source}")
configure_file("${b3_generated_dir}/btc_a2dp_source.c.in" "${b3_generated_source}" COPYONLY)
get_target_property(b3_bt_sources ${b3_bt_lib} SOURCES)
list(FIND b3_bt_sources "${b3_sdk_source}" b3_source_index)
if(b3_source_index EQUAL -1)
    message(FATAL_ERROR "B3 A2DP source was not found in the Bluetooth target.")
endif()
list(REMOVE_AT b3_bt_sources ${b3_source_index})
list(APPEND b3_bt_sources "${b3_generated_source}")
set_property(TARGET ${b3_bt_lib} PROPERTY SOURCES "${b3_bt_sources}")
target_compile_definitions(${b3_main_lib} PRIVATE
    B3_LOW_LATENCY_A2DP=1 B3_A2DP_MEDIA_TICK_MS=${b3_media_tick_ms})
message(STATUS "B3 audio: ${b3_media_tick_ms} ms A2DP cycle, ${b3_tx_queue_packets} encoded packets, project-only SDK patch")

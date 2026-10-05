/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-FileCopyrightText: 2026 Make2Hear contributors
 *
 * SPDX-License-Identifier: GPL-3.0-only AND (Unlicense OR CC0-1.0)
 *
 * Original Espressif portions: Unlicense OR CC0-1.0.
 * Make2Hear modifications: GPL-3.0-only. See THIRD_PARTY_NOTICES.md.
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"

#include "esp_bt.h"
#include "bt_app_core.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

/* log tags */
#define BT_AV_TAG             "BT_AV"
#define BT_RC_CT_TAG          "RC_CT"

/* device name */
#define LOCAL_DEVICE_NAME     CONFIG_MAKE2HEAR_DEVICE_NAME

/* INMP441 -> ESP32: VDD -> 3.3 V, GND -> GND, SCK -> BCLK,
 * WS -> WS, SD -> DATA. Tie L/R to GND for the left slot used below;
 * if L/R is tied to 3.3 V, select the right slot in menuconfig. */
#define I2S_MIC_BCLK         CONFIG_MAKE2HEAR_MIC_BCLK
#define I2S_MIC_WS           CONFIG_MAKE2HEAR_MIC_WS
#define I2S_MIC_DATA         CONFIG_MAKE2HEAR_MIC_DATA
#define AUDIO_SAMPLE_RATE    44100
#define MIC_GAIN             CONFIG_MAKE2HEAR_MIC_GAIN
#define HPF_CUTOFF_HZ        100.0f
#define LIMITER_PEAK_PCM     28000   /* Leave headroom below int16_t full scale. */
#define LIMITER_RELEASE_MS   80      /* Gain recovery time; does not delay samples. */
#define AUDIO_STATS_PERIOD_MS CONFIG_MAKE2HEAR_STATS_PERIOD_MS
#if B3_LOW_LATENCY_A2DP
#define I2S_READ_SAMPLES     64
#define I2S_DMA_BUFFERS      6
#define AUDIO_BUFFER_SAMPLES 640
#else
/* Build with -DB3_LOW_LATENCY_A2DP=OFF for an A/B test of the old buffering. */
#define I2S_READ_SAMPLES     128
#define I2S_DMA_BUFFERS      3
#define AUDIO_BUFFER_SAMPLES 1536
#endif
#define MIC_TASK_PRIORITY    18
#if CONFIG_FREERTOS_UNICORE
#define MIC_TASK_CORE        0
#else
#define MIC_TASK_CORE        1
#endif

/* The project build changes IDF's SBC cycle to 10 ms (normally 3-4 frames).
 * 640 samples hold 14.51 ms: four 128-sample frames plus 128 samples of margin.
 * Keep the ring and SDK cycle coupled; shrinking only the ring causes gaps. */
_Static_assert(AUDIO_SAMPLE_RATE == 44100, "Must match the preferred SBC rate below");
_Static_assert(AUDIO_BUFFER_SAMPLES >= I2S_READ_SAMPLES, "Buffer must fit one I2S read");
_Static_assert(AUDIO_BUFFER_SAMPLES >=
               ((AUDIO_SAMPLE_RATE * B3_A2DP_MEDIA_TICK_MS + 128000 - 1) / 128000) * 128 + 128,
               "PCM ring must fit a normal SBC burst plus scheduling margin");
_Static_assert(MIC_GAIN == 1 || MIC_GAIN == 2 || MIC_GAIN == 4 || MIC_GAIN == 8,
               "Use MIC_GAIN 1, 2, 4 or 8");
_Static_assert(LIMITER_PEAK_PCM > 0 && LIMITER_PEAK_PCM <= 32767, "Invalid limiter ceiling");
_Static_assert(LIMITER_RELEASE_MS > 0 && LIMITER_RELEASE_MS <= 1000, "Invalid limiter release");

/* AVRCP used transaction label */
#define APP_RC_CT_TL_GET_CAPS            (0)
#define APP_RC_CT_TL_RN_VOLUME_CHANGE    (1)

enum {
    BT_APP_STACK_UP_EVT   = 0x0000,    /* event for stack up */
    BT_APP_HEART_BEAT_EVT = 0xff00,    /* event for heart beat */
    BT_APP_AUDIO_STATS_EVT = 0xff01,   /* diagnostic event, independent of connection timer */
};

/* A2DP global states */
enum {
    APP_AV_STATE_IDLE,
    APP_AV_STATE_DISCOVERING,
    APP_AV_STATE_DISCOVERED,
    APP_AV_STATE_UNCONNECTED,
    APP_AV_STATE_CONNECTING,
    APP_AV_STATE_CONNECTED,
    APP_AV_STATE_DISCONNECTING,
};

/* sub states of APP_AV_STATE_CONNECTED */
enum {
    APP_AV_MEDIA_STATE_IDLE,
    APP_AV_MEDIA_STATE_STARTING,
    APP_AV_MEDIA_STATE_STARTED,
};

/*********************************
 * STATIC FUNCTION DECLARATIONS
 ********************************/

/* handler for bluetooth stack enabled events */
static void bt_av_hdl_stack_evt(uint16_t event, void *p_param);

/* avrc controller event handler */
static void bt_av_hdl_avrc_ct_evt(uint16_t event, void *p_param);

/* GAP callback function */
static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);

/* callback function for A2DP source */
static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);

/* callback function for A2DP source audio data stream */
static int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len);

/* callback function for AVRCP controller */
static void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);

/* handler for heart beat timer */
static void bt_app_a2d_heart_beat(TimerHandle_t arg);
static void audio_stats_timer_cb(TimerHandle_t arg);

/* A2DP application state machine */
static void bt_app_av_sm_hdlr(uint16_t event, void *param);

/* utils for transfer BLuetooth Deveice Address into string form */
static char *bda2str(esp_bd_addr_t bda, char *str, size_t size);

/* check preferred codec configuration against sink capabilities */
static bool check_pref_mcc_against_sink_caps(const esp_a2d_mcc_t *sink_caps, const esp_a2d_mcc_t *pref_mcc);

/* set preferred codec configuration */
static void bt_app_a2d_set_pref_mcc(esp_a2d_conn_hdl_t conn_hdl, const esp_a2d_mcc_t *sink_caps);

/* A2DP application state machine handler for each state */
static void bt_app_av_state_unconnected_hdlr(uint16_t event, void *param);
static void bt_app_av_state_connecting_hdlr(uint16_t event, void *param);
static void bt_app_av_state_connected_hdlr(uint16_t event, void *param);
static void bt_app_av_state_disconnecting_hdlr(uint16_t event, void *param);

static void init_i2s_microphone(void);
static void microphone_task(void *arg);
static void log_audio_stats(void);
/* Called only when the project-patched SBC source discards a stale packet. */
void app_audio_tx_packet_dropped(void);

/*********************************
 * STATIC VARIABLE DEFINITIONS
 ********************************/

static esp_bd_addr_t s_peer_bda = {0};                        /* Bluetooth Device Address of peer device*/
static uint8_t s_peer_bdname[ESP_BT_GAP_MAX_BDNAME_LEN + 1];  /* Bluetooth Device Name of peer device*/
static int s_a2d_state = APP_AV_STATE_IDLE;                   /* A2DP global state */
static int s_media_state = APP_AV_MEDIA_STATE_IDLE;           /* sub states of APP_AV_STATE_CONNECTED */
static int s_connecting_intv = 0;                             /* count of heart beat intervals for connecting */
static uint32_t s_pkt_cnt = 0;                                /* count of packets */
static esp_avrc_rn_evt_cap_mask_t s_avrc_peer_rn_cap;         /* AVRC target notification event capability bit mask */
static TimerHandle_t s_tmr;                                   /* handle of heart beat timer */
static i2s_chan_handle_t s_mic_rx;

/* One producer (microphone task), one consumer (A2DP media task). The short
 * critical sections protect both cores; no I2S, DSP or logging runs inside. */
static portMUX_TYPE s_audio_lock = portMUX_INITIALIZER_UNLOCKED;
static int16_t s_audio_buffer[AUDIO_BUFFER_SAMPLES];
static size_t s_audio_read_pos;
static size_t s_audio_count;
static bool s_pcm_format_confirmed;
static bool s_audio_streaming;
static int64_t s_audio_last_callback_us; /* Protected by s_audio_lock. */

typedef struct {
    uint32_t read_failures;
    uint32_t dma_overruns;
    uint32_t underruns;
    uint32_t missing_samples;
    uint32_t dropped_samples;
    uint32_t clipped_samples;
    uint32_t limited_samples;
    uint32_t input_near_fs;  /* Raw input >=99% full scale; possible ADC overload. */
    uint32_t sample_count;
    uint32_t raw_peak24;     /* All input bits, before filter and gain. */
    uint32_t pre_gain_peak24;
    uint64_t pre_gain_abs_sum24; /* Sum after HPF, before gain; no sample history needed. */
    uint32_t pcm_peak;       /* After filter/gain/saturation, up to 32768. */
    uint32_t tx_dropped_packets; /* Source SBC queue only, not RF packet loss. */
    uint32_t queue_high_samples;
    uint32_t processing_max_us;  /* Read-return -> PCM ready, includes preemption. */
    uint32_t callback_count;
    uint32_t callback_max_bytes;
    uint32_t callback_max_us;
    uint32_t callback_gap_max_us;
} audio_stats_t;
static audio_stats_t s_audio_stats;

static const char remote_device_name[] = CONFIG_EXAMPLE_PEER_DEVICE_NAME;

/*********************************
 * STATIC FUNCTION DEFINITIONS
 ********************************/

static char *bda2str(esp_bd_addr_t bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }

    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return str;
}

static bool get_name_from_eir(uint8_t *eir, uint8_t *bdname, uint8_t *bdname_len)
{
    uint8_t *rmt_bdname = NULL;
    uint8_t rmt_bdname_len = 0;

    if (!eir) {
        return false;
    }

    /* get complete or short local name from eir data */
    rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname) {
        rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }

    if (rmt_bdname) {
        if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
            rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
        }

        if (bdname) {
            memcpy(bdname, rmt_bdname, rmt_bdname_len);
            bdname[rmt_bdname_len] = '\0';
        }
        if (bdname_len) {
            *bdname_len = rmt_bdname_len;
        }
        return true;
    }

    return false;
}

static void filter_inquiry_scan_result(esp_bt_gap_cb_param_t *param)
{
    char bda_str[18];
    uint32_t cod = 0;     /* class of device */
    int32_t rssi = -129;  /* invalid value */
    uint8_t *eir = NULL;
    uint8_t candidate_name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = {0};
    esp_bt_gap_dev_prop_t *p;

    /* handle the discovery results */
    ESP_LOGI(BT_AV_TAG, "Scanned device: %s", bda2str(param->disc_res.bda, bda_str, 18));
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        p = param->disc_res.prop + i;
        switch (p->type) {
        case ESP_BT_GAP_DEV_PROP_COD:
            cod = *(uint32_t *)(p->val);
            ESP_LOGI(BT_AV_TAG, "--Class of Device: 0x%"PRIx32, cod);
            break;
        case ESP_BT_GAP_DEV_PROP_RSSI:
            rssi = *(int8_t *)(p->val);
            ESP_LOGI(BT_AV_TAG, "--RSSI: %"PRId32, rssi);
            break;
        case ESP_BT_GAP_DEV_PROP_EIR:
            eir = (uint8_t *)(p->val);
            break;
        case ESP_BT_GAP_DEV_PROP_BDNAME:
            if (p->val && p->len > 0) {
                size_t length = p->len;
                if (length > ESP_BT_GAP_MAX_BDNAME_LEN) {
                    length = ESP_BT_GAP_MAX_BDNAME_LEN;
                }
                memcpy(candidate_name, p->val, length);
                candidate_name[length] = '\0';
            }
            break;
        default:
            break;
        }
    }

    /* search for device with MAJOR service class as "rendering" in COD */
    if (!esp_bt_gap_is_valid_cod(cod) ||
            !(esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING)) {
        return;
    }

    /* Each result owns its name: a missing EIR name must not reuse an earlier
     * device's name. The separate BDNAME property is a useful fallback. */
    bool have_name = candidate_name[0] != '\0';
    if (!have_name && eir) {
        have_name = get_name_from_eir(eir, candidate_name, NULL);
    }
    if (have_name && strcmp((char *)candidate_name, remote_device_name) == 0) {
        memcpy(s_peer_bdname, candidate_name, sizeof(s_peer_bdname));
        ESP_LOGI(BT_AV_TAG, "Found a target device, address %s, name %s", bda_str, s_peer_bdname);
        s_a2d_state = APP_AV_STATE_DISCOVERED;
        memcpy(s_peer_bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
        ESP_LOGI(BT_AV_TAG, "Cancel device discovery ...");
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bt_gap_cancel_discovery());
    }
}

static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    /* when device discovered a result, this event comes */
    case ESP_BT_GAP_DISC_RES_EVT: {
        if (s_a2d_state == APP_AV_STATE_DISCOVERING) {
            filter_inquiry_scan_result(param);
        }
        break;
    }
    /* when discovery state changed, this event comes */
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            if (s_a2d_state == APP_AV_STATE_DISCOVERED) {
                s_a2d_state = APP_AV_STATE_CONNECTING;
                ESP_LOGI(BT_AV_TAG, "Device discovery stopped.");
                ESP_LOGI(BT_AV_TAG, "a2dp connecting to peer: %s", s_peer_bdname);
                /* connect source to peer device specified by Bluetooth Device Address */
                esp_err_t err = esp_a2d_source_connect(s_peer_bda);
                if (err != ESP_OK) {
                    ESP_LOGE(BT_AV_TAG, "Connection request failed: %s; will retry", esp_err_to_name(err));
                    s_a2d_state = APP_AV_STATE_UNCONNECTED;
                }
            } else {
                /* not discovered, continue to discover */
                ESP_LOGI(BT_AV_TAG, "Device discovery failed, continue to discover...");
                ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0));
            }
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            ESP_LOGI(BT_AV_TAG, "Discovery started.");
        }
        break;
    }
    /* when authentication completed, this event comes */
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(BT_AV_TAG, "authentication success: %s", param->auth_cmpl.device_name);
            ESP_LOG_BUFFER_HEX(BT_AV_TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
        } else {
            ESP_LOGE(BT_AV_TAG, "authentication failed, status: %d", param->auth_cmpl.stat);
        }
        break;
    }
    /* when Legacy Pairing pin code requested, this event comes */
    case ESP_BT_GAP_PIN_REQ_EVT: {
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_PIN_REQ_EVT min_16_digit: %d", param->pin_req.min_16_digit);
        esp_bt_pin_code_t pin_code = {0};
        size_t length = strlen(CONFIG_MAKE2HEAR_LEGACY_PIN);
        bool accept = length > 0 && length <= sizeof(pin_code) &&
                      (!param->pin_req.min_16_digit || length == 16) &&
                      memcmp(param->pin_req.bda, s_peer_bda, ESP_BD_ADDR_LEN) == 0;
        if (accept) {
            memcpy(pin_code, CONFIG_MAKE2HEAR_LEGACY_PIN, length);
        } else {
            ESP_LOGW(BT_AV_TAG, "Legacy pairing rejected; check target and configured PIN length");
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bt_gap_pin_reply(param->pin_req.bda, accept,
                                                          accept ? length : 0, pin_code));
        break;
    }

#if (CONFIG_EXAMPLE_SSP_ENABLED == true)
    /* when Security Simple Pairing user confirmation requested, this event comes */
    case ESP_BT_GAP_CFM_REQ_EVT:
        /* Headless Just Works pairing: accept only the selected headphones.
         * There is no display/keypad for numeric comparison. */
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda,
            memcmp(param->cfm_req.bda, s_peer_bda, ESP_BD_ADDR_LEN) == 0));
        break;
    /* when Security Simple Pairing passkey notified, this event comes */
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey: %06"PRIu32, param->key_notif.passkey);
        break;
    /* when Security Simple Pairing passkey requested, this event comes */
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGW(BT_AV_TAG, "Passkey entry is unsupported on this headless prototype");
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_bt_gap_ssp_passkey_reply(param->key_req.bda, false, 0));
        break;
#endif

    /* when GAP mode changed, this event comes */
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_MODE_CHG_EVT mode: %d", param->mode_chg.mode);
        break;
    case ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT:
        if (param->get_dev_name_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT device name: %s", param->get_dev_name_cmpl.name);
        } else {
            ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_GET_DEV_NAME_CMPL_EVT failed, state: %d", param->get_dev_name_cmpl.status);
        }
        break;
    /* other */
    default: {
        ESP_LOGI(BT_AV_TAG, "event: %d", event);
        break;
    }
    }

    return;
}

static void bt_av_hdl_stack_evt(uint16_t event, void *p_param)
{
    ESP_LOGD(BT_AV_TAG, "%s event: %d", __func__, event);

    switch (event) {
    /* when stack up worked, this event comes */
    case BT_APP_STACK_UP_EVT: {
        ESP_ERROR_CHECK(esp_bt_gap_set_device_name(LOCAL_DEVICE_NAME));
        ESP_ERROR_CHECK(esp_bt_gap_register_callback(bt_app_gap_cb));

        ESP_ERROR_CHECK(esp_avrc_ct_init());
        ESP_ERROR_CHECK(esp_avrc_ct_register_callback(bt_app_rc_ct_cb));

        ESP_ERROR_CHECK(esp_a2d_source_init());
        ESP_ERROR_CHECK(esp_a2d_register_callback(&bt_app_a2d_cb));
        ESP_ERROR_CHECK(esp_a2d_source_register_data_callback(bt_app_a2d_data_cb));

        /* Avoid the state error of s_a2d_state caused by the connection initiated by the peer device. */
        ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE));

        ESP_LOGI(BT_AV_TAG, "Starting device discovery for '%s'...", remote_device_name);
        s_a2d_state = APP_AV_STATE_DISCOVERING;
        ESP_ERROR_CHECK(esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0));

        /* create and start heart beat timer */
        s_tmr = xTimerCreate("connTmr", pdMS_TO_TICKS(10000), pdTRUE, NULL, bt_app_a2d_heart_beat);
        ESP_ERROR_CHECK(s_tmr != NULL ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(xTimerStart(s_tmr, 0) == pdPASS ? ESP_OK : ESP_FAIL);

        /* Dispatch diagnostics to the app task; UART output must not block capture. */
        TimerHandle_t stats_timer = xTimerCreate("audioStats", pdMS_TO_TICKS(AUDIO_STATS_PERIOD_MS),
                                                pdTRUE, NULL, audio_stats_timer_cb);
        ESP_ERROR_CHECK(stats_timer != NULL ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(xTimerStart(stats_timer, 0) == pdPASS ? ESP_OK : ESP_FAIL);
        break;
    }
    /* other */
    default: {
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
    }
}

static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    /* A preferred configuration is a request, not a guarantee. IDF derives
     * the callback's channel count from the accepted SBC configuration. */
    if (event == ESP_A2D_SRC_SET_PREF_MCC_EVT ||
            (event == ESP_A2D_CONNECTION_STATE_EVT &&
             param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED)) {
        bool confirmed = event == ESP_A2D_SRC_SET_PREF_MCC_EVT &&
                         param->a2d_set_pref_mcc_stat.set_status == ESP_BT_STATUS_SUCCESS;
        portENTER_CRITICAL(&s_audio_lock);
        s_pcm_format_confirmed = confirmed;
        s_audio_read_pos = 0;
        s_audio_count = 0;
        s_audio_streaming = false;
        portEXIT_CRITICAL(&s_audio_lock);
        if (event == ESP_A2D_SRC_SET_PREF_MCC_EVT) {
            if (confirmed) {
                ESP_LOGI(BT_AV_TAG, "PCM confirmed: 44100 Hz, signed 16-bit mono");
            } else {
                ESP_LOGE(BT_AV_TAG, "PCM format unconfirmed; sending silence");
            }
        }
    } else if (event == ESP_A2D_AUDIO_STATE_EVT) {
        portENTER_CRITICAL(&s_audio_lock);
        s_audio_streaming = param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        s_audio_last_callback_us = 0;
        portEXIT_CRITICAL(&s_audio_lock);
    }
    if (!bt_app_work_dispatch(bt_app_av_sm_hdlr, event, param, sizeof(esp_a2d_cb_param_t), NULL, NULL)) {
        ESP_LOGE(BT_AV_TAG, "Lost A2DP event %d; connection state may be stale", event);
    }
}

static bool microphone_dma_overrun(i2s_chan_handle_t handle, i2s_event_data_t *event, void *arg)
{
    /* DMA queue overflow can drop samples even when the next read succeeds. */
    portENTER_CRITICAL_ISR(&s_audio_lock);
    s_audio_stats.dma_overruns++;
    portEXIT_CRITICAL_ISR(&s_audio_lock);
    return false;
}

void app_audio_tx_packet_dropped(void)
{
    portENTER_CRITICAL(&s_audio_lock);
    s_audio_stats.tx_dropped_packets++;
    portEXIT_CRITICAL(&s_audio_lock);
}

static void init_i2s_microphone(void)
{
    ESP_ERROR_CHECK(GPIO_IS_VALID_OUTPUT_GPIO(I2S_MIC_BCLK) &&
                    GPIO_IS_VALID_OUTPUT_GPIO(I2S_MIC_WS) && GPIO_IS_VALID_GPIO(I2S_MIC_DATA) &&
                    I2S_MIC_BCLK != I2S_MIC_WS && I2S_MIC_DATA != I2S_MIC_BCLK &&
                    I2S_MIC_DATA != I2S_MIC_WS ? ESP_OK : ESP_ERR_INVALID_ARG);
    /* Called by the pinned reader task: I2S allocates its interrupt on the
     * calling core. On this dual-core ESP32, capture runs on core 1 while
     * the configured Bluetooth controller/host run on core 0. */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_frame_num = I2S_READ_SAMPLES;
    /* Low-latency mode: 1.45 ms blocks, still 8.71 ms total DMA capacity.
     * More small descriptors preserve tolerance for brief scheduling delays;
     * the task drains each completed block without waiting for all of them. */
    chan_cfg.dma_desc_num = I2S_DMA_BUFFERS;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_mic_rx));

    i2s_std_config_t std_cfg = {
        /* Match the preferred A2DP rate so no sample-rate conversion is needed. */
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        /* Two 32-clock slots give INMP441 its required 64 BCLKs per WS frame.
         * Mono RX stores only the selected slot, still clocking both slots. */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_MIC_BCLK,
            .ws = I2S_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_MIC_DATA,
        },
    };
    /* APLL provides a more accurate audio clock, reducing long-term drift
     * against the SBC consumption rate without any resampling. */
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
#if CONFIG_MAKE2HEAR_MIC_RIGHT_SLOT
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
#else
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
#endif
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_mic_rx, &std_cfg));
    i2s_event_callbacks_t callbacks = {.on_recv_q_ovf = microphone_dma_overrun};
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(s_mic_rx, &callbacks, NULL));
    ESP_LOGI(BT_AV_TAG, "I2S microphone initialized at %d Hz; fixed gain %dx, HPF %.0f Hz, limiter %d, core %d, priority %d",
             AUDIO_SAMPLE_RATE, MIC_GAIN, (double)HPF_CUTOFF_HZ, LIMITER_PEAK_PCM,
             xPortGetCoreID(), MIC_TASK_PRIORITY);
    ESP_LOGI(BT_AV_TAG, "Audio buffering: A2DP cycle=%d ms, I2S block=%d samples, DMA=%d blocks, PCM=%d samples (%.2f ms)",
             B3_A2DP_MEDIA_TICK_MS, I2S_READ_SAMPLES, I2S_DMA_BUFFERS,
             AUDIO_BUFFER_SAMPLES, 1000.0 * AUDIO_BUFFER_SAMPLES / AUDIO_SAMPLE_RATE);
    /* Start DMA after the startup log so UART output cannot delay its first read. */
    ESP_ERROR_CHECK(i2s_channel_enable(s_mic_rx));
}

static int16_t apply_gain_and_limit(int32_t sample24, audio_stats_t *stats)
{
    /* Multiply BEFORE reducing to 16 bits, so quiet signals can use their
     * low-order bits. int64_t also leaves room for filter transients and gain. */
    int64_t amplified = (int64_t)sample24 * MIC_GAIN;
    int64_t magnitude = amplified < 0 ? -amplified : amplified;
    const int64_t ceiling24 = (int64_t)LIMITER_PEAK_PCM * 256;

    /* 32768 means full requested gain; smaller values attenuate it. This
     * limiter only REDUCES gain, never boosts quiet speech or room noise.
     * Attack is immediate to catch the current peak without look-ahead. */
    static int32_t limiter_gain = 32768;
    int32_t allowed_gain = 32768;
    if (magnitude > ceiling24) {
        allowed_gain = (int32_t)(ceiling24 * 32768 / magnitude);
    }
    if (allowed_gain < limiter_gain) {
        limiter_gain = allowed_gain;
    } else {
        const int32_t release_samples = AUDIO_SAMPLE_RATE * LIMITER_RELEASE_MS / 1000;
        /* Gradual recovery avoids jumping straight back to full volume.
         * Round the step up so integer rounding cannot leave gain stuck. */
        limiter_gain += (32768 - limiter_gain + release_samples - 1) / release_samples;
        if (limiter_gain > allowed_gain) {
            limiter_gain = allowed_gain;
        }
    }
    if (limiter_gain < 32768) {
        stats->limited_samples++;
    }
    int64_t limited24 = amplified * limiter_gain / 32768;
    int64_t pcm = limited24 >= 0 ? (limited24 + 128) / 256 : -((-limited24 + 128) / 256);

    /* Final saturation is a safety guard; no signed integer wraparound. */
    if (pcm > 32767) {
        pcm = 32767;
        stats->clipped_samples++;
    } else if (pcm < -32768) {
        pcm = -32768;
        stats->clipped_samples++;
    }
    uint32_t pcm_abs = (uint32_t)(pcm < 0 ? -pcm : pcm);
    if (pcm_abs > stats->pcm_peak) {
        stats->pcm_peak = pcm_abs;
    }
    return (int16_t)pcm;
}

static int16_t process_audio_sample(int32_t raw, audio_stats_t *stats)
{
    static float previous_input;
    static float previous_output;
    const float alpha = AUDIO_SAMPLE_RATE / (AUDIO_SAMPLE_RATE + 6.2831853f * HPF_CUTOFF_HZ);

    /* Philips RX puts the signed 24-bit signal in bits 31:8. Bits 7:0 are
     * outside the microphone word; discard only those, not quiet signal bits.
     * Work in 24-bit units through HPF and gain; scale to int16_t only at the end. */
    int32_t sample24 = raw >> 8;
    uint32_t raw_abs = (uint32_t)(sample24 < 0 ? -sample24 : sample24);
    stats->sample_count++;
    if (raw_abs > stats->raw_peak24) {
        stats->raw_peak24 = raw_abs;
    }
    if (raw_abs >= 8304721) { /* 99% of 2^23: a near-rail warning, not proof of clipping. */
        stats->input_near_fs++;
    }
    float input = (float)sample24;
    /* First-order high-pass: removes DC/rumble with no look-ahead buffer. */
    float filtered = alpha * (previous_output + input - previous_input);
    previous_input = input;
    previous_output = filtered;
    int32_t filtered24 = (int32_t)(filtered + (filtered >= 0 ? 0.5f : -0.5f));
    uint32_t filtered_abs = filtered24 < 0 ? -filtered24 : filtered24;
    if (filtered_abs > stats->pre_gain_peak24) {
        stats->pre_gain_peak24 = filtered_abs;
    }
    stats->pre_gain_abs_sum24 += filtered_abs;
    return apply_gain_and_limit(filtered24, stats);
}

static void microphone_task(void *arg)
{
    init_i2s_microphone();
    int32_t mic[I2S_READ_SAMPLES];
    int16_t pcm[I2S_READ_SAMPLES];

    while (true) {
        size_t bytes_read = 0;
        /* Only this task waits on DMA; the A2DP callback must return promptly. */
        esp_err_t err = i2s_channel_read(s_mic_rx, mic, sizeof(mic), &bytes_read, 20);
        int64_t processing_start_us = esp_timer_get_time();
        size_t count = bytes_read / sizeof(mic[0]);
        audio_stats_t block = {0};
        for (size_t i = 0; i < count; i++) {
            pcm[i] = process_audio_sample(mic[i], &block);
        }
        uint32_t processing_us = (uint32_t)(esp_timer_get_time() - processing_start_us);

        portENTER_CRITICAL(&s_audio_lock);
        if (processing_us > s_audio_stats.processing_max_us) {
            s_audio_stats.processing_max_us = processing_us;
        }
        s_audio_stats.read_failures += err != ESP_OK || bytes_read != sizeof(mic);
        s_audio_stats.clipped_samples += block.clipped_samples;
        s_audio_stats.limited_samples += block.limited_samples;
        s_audio_stats.input_near_fs += block.input_near_fs;
        s_audio_stats.sample_count += block.sample_count;
        s_audio_stats.pre_gain_abs_sum24 += block.pre_gain_abs_sum24;
        if (block.raw_peak24 > s_audio_stats.raw_peak24) {
            s_audio_stats.raw_peak24 = block.raw_peak24;
        }
        if (block.pre_gain_peak24 > s_audio_stats.pre_gain_peak24) {
            s_audio_stats.pre_gain_peak24 = block.pre_gain_peak24;
        }
        if (block.pcm_peak > s_audio_stats.pcm_peak) {
            s_audio_stats.pcm_peak = block.pcm_peak;
        }
        /* Keep the newest PCM if Bluetooth pauses. This bounds this ring's
         * backlog; encoded Bluetooth/headphone queues are separate.
         * Overwrites while disconnected are expected and are not counted. */
        size_t free_samples = AUDIO_BUFFER_SAMPLES - s_audio_count;
        if (count > free_samples) {
            size_t drop = count - free_samples;
            s_audio_read_pos = (s_audio_read_pos + drop) % AUDIO_BUFFER_SAMPLES;
            s_audio_count -= drop;
            if (s_audio_streaming && s_pcm_format_confirmed) {
                s_audio_stats.dropped_samples += drop;
            }
        }
        size_t write_pos = (s_audio_read_pos + s_audio_count) % AUDIO_BUFFER_SAMPLES;
        size_t first = AUDIO_BUFFER_SAMPLES - write_pos;
        if (first > count) {
            first = count;
        }
        memcpy(&s_audio_buffer[write_pos], pcm, first * sizeof(pcm[0]));
        memcpy(s_audio_buffer, pcm + first, (count - first) * sizeof(pcm[0]));
        s_audio_count += count;
        if (s_audio_streaming && s_audio_count > s_audio_stats.queue_high_samples) {
            s_audio_stats.queue_high_samples = s_audio_count;
        }
        portEXIT_CRITICAL(&s_audio_lock);

        /* An invalid channel can fail immediately; avoid a busy error loop. */
        if (count == 0 && err != ESP_ERR_TIMEOUT) {
            vTaskDelay(1);
        }
    }
}

static void log_audio_stats(void)
{
    portENTER_CRITICAL(&s_audio_lock);
    audio_stats_t stats = s_audio_stats;
    size_t queued = s_audio_count;
    bool format_ok = s_pcm_format_confirmed;
    memset(&s_audio_stats, 0, sizeof(s_audio_stats));
    portEXIT_CRITICAL(&s_audio_lock);
    /* Scale ONLY for display: peaks and average use 16-bit-equivalent units,
     * with fractions for quiet signals. A full-scale input is about 32768.
     * This is mean absolute amplitude, not RMS. No buffered analysis window. */
    double pre_avg = stats.sample_count ? (double)stats.pre_gain_abs_sum24 / stats.sample_count / 256.0 : 0;
    ESP_LOGI(BT_AV_TAG, "Audio/%ums: fmt=%d gain=%dx n=%"PRIu32
             " raw_pk=%.2f pre_pk=%.2f pre_avg=%.2f in_near_fs=%"PRIu32
             " limited=%"PRIu32" clip=%"PRIu32" pcm_pk=%"PRIu32
             " read_fail=%"PRIu32" dma_overrun=%"PRIu32" underrun=%"PRIu32
             " missing=%"PRIu32" drop=%"PRIu32" queued=%u",
             (unsigned)AUDIO_STATS_PERIOD_MS, format_ok, MIC_GAIN, stats.sample_count,
             stats.raw_peak24 / 256.0, stats.pre_gain_peak24 / 256.0, pre_avg, stats.input_near_fs,
             stats.limited_samples, stats.clipped_samples, stats.pcm_peak,
             stats.read_failures, stats.dma_overruns, stats.underruns, stats.missing_samples, stats.dropped_samples,
             (unsigned)queued);
    /* Runtime/occupancy measurements, not a claim of end-to-end latency.
     * Callback gaps include the quiet interval between encoder bursts. */
    ESP_LOGI(BT_AV_TAG, "Timing/%ums: q_high=%"PRIu32" (%.2f ms) dsp_max=%"PRIu32
             " us cb_n=%"PRIu32" cb_bytes_max=%"PRIu32" cb_max=%"PRIu32
             " us cb_gap_max=%"PRIu32" us tx_drop=%"PRIu32,
             (unsigned)AUDIO_STATS_PERIOD_MS, stats.queue_high_samples,
             1000.0 * stats.queue_high_samples / AUDIO_SAMPLE_RATE, stats.processing_max_us,
             stats.callback_count, stats.callback_max_bytes, stats.callback_max_us,
             stats.callback_gap_max_us, stats.tx_dropped_packets);
}

/* Confirmed SBC mono needs ONE int16_t per sample. Duplicating L/R here
 * would repeat samples and halve the pitch; stereo would need a different format. */
static int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len)
{
    if (len == -1) {
        /* IDF asks us to flush on stream reset; don't replay the old backlog. */
        portENTER_CRITICAL(&s_audio_lock);
        s_audio_read_pos = 0;
        s_audio_count = 0;
        s_audio_last_callback_us = 0;
        portEXIT_CRITICAL(&s_audio_lock);
        return 0;
    }
    if (data == NULL || len <= 0) {
        return 0;
    }
    int64_t callback_start_us = esp_timer_get_time();

    /* All unavailable bytes, including an odd trailing byte, remain silence. */
    memset(data, 0, len);
    size_t requested = len / sizeof(int16_t);
    portENTER_CRITICAL(&s_audio_lock);
    if (s_audio_streaming) {
        s_audio_stats.callback_count++;
        if ((uint32_t)len > s_audio_stats.callback_max_bytes) {
            s_audio_stats.callback_max_bytes = (uint32_t)len;
        }
        if (s_audio_last_callback_us != 0) {
            int64_t gap_us = callback_start_us - s_audio_last_callback_us;
            if (gap_us > s_audio_stats.callback_gap_max_us) {
                s_audio_stats.callback_gap_max_us = gap_us > UINT32_MAX ? UINT32_MAX : (uint32_t)gap_us;
            }
        }
        s_audio_last_callback_us = callback_start_us;
    } else {
        s_audio_last_callback_us = 0;
    }
    if (s_pcm_format_confirmed) {
        size_t count = requested < s_audio_count ? requested : s_audio_count;
        size_t first = AUDIO_BUFFER_SAMPLES - s_audio_read_pos;
        if (first > count) {
            first = count;
        }
        memcpy(data, &s_audio_buffer[s_audio_read_pos], first * sizeof(int16_t));
        memcpy(data + first * sizeof(int16_t), s_audio_buffer, (count - first) * sizeof(int16_t));
        s_audio_read_pos = (s_audio_read_pos + count) % AUDIO_BUFFER_SAMPLES;
        s_audio_count -= count;
        if (count < requested) {
            s_audio_stats.underruns++;
            s_audio_stats.missing_samples += requested - count;
        }
    }
    uint32_t callback_us = (uint32_t)(esp_timer_get_time() - callback_start_us);
    if (callback_us > s_audio_stats.callback_max_us) {
        s_audio_stats.callback_max_us = callback_us;
    }
    portEXIT_CRITICAL(&s_audio_lock);

    return len;
}

static void bt_app_a2d_heart_beat(TimerHandle_t arg)
{
    bt_app_post_timer_event(bt_app_av_sm_hdlr, BT_APP_HEART_BEAT_EVT);
}

static void audio_stats_timer_cb(TimerHandle_t arg)
{
    bt_app_post_timer_event(bt_app_av_sm_hdlr, BT_APP_AUDIO_STATS_EVT);
}

static void bt_app_av_sm_hdlr(uint16_t event, void *param)
{
    if (event == BT_APP_AUDIO_STATS_EVT) {
        log_audio_stats();
        return;
    }
    ESP_LOGI(BT_AV_TAG, "%s state: %d, event: 0x%x", __func__, s_a2d_state, event);

    /* select handler according to different states */
    switch (s_a2d_state) {
    case APP_AV_STATE_DISCOVERING:
    case APP_AV_STATE_DISCOVERED:
        break;
    case APP_AV_STATE_UNCONNECTED:
        bt_app_av_state_unconnected_hdlr(event, param);
        break;
    case APP_AV_STATE_CONNECTING:
        bt_app_av_state_connecting_hdlr(event, param);
        break;
    case APP_AV_STATE_CONNECTED:
        bt_app_av_state_connected_hdlr(event, param);
        break;
    case APP_AV_STATE_DISCONNECTING:
        bt_app_av_state_disconnecting_hdlr(event, param);
        break;
    default:
        ESP_LOGE(BT_AV_TAG, "%s invalid state: %d", __func__, s_a2d_state);
        break;
    }
}

static bool is_one_bit_set_u8(uint8_t v)
{
    return (v != 0) && ((v & (uint8_t)(v - 1)) == 0);
}

static bool check_pref_mcc_against_sink_caps(const esp_a2d_mcc_t *sink_caps, const esp_a2d_mcc_t *pref_mcc)
{
    const esp_a2d_cie_sbc_t *caps;
    const esp_a2d_cie_sbc_t *cfg;

    if (sink_caps == NULL || pref_mcc == NULL) {
        return false;
    }
    if (sink_caps->type != pref_mcc->type) {
        return false;
    }

    if (pref_mcc->type != ESP_A2D_MCT_SBC) {
        return false;
    }

    caps = &sink_caps->cie.sbc_info;
    cfg = &pref_mcc->cie.sbc_info;

    /* For preferred configuration, each field should select a single value (one bit) */
    if (!is_one_bit_set_u8(cfg->samp_freq) || ((cfg->samp_freq & caps->samp_freq) != cfg->samp_freq)) {
        return false;
    }
    if (!is_one_bit_set_u8(cfg->ch_mode) || ((cfg->ch_mode & caps->ch_mode) != cfg->ch_mode)) {
        return false;
    }
    if (!is_one_bit_set_u8(cfg->block_len) || ((cfg->block_len & caps->block_len) != cfg->block_len)) {
        return false;
    }
    if (!is_one_bit_set_u8(cfg->num_subbands) || ((cfg->num_subbands & caps->num_subbands) != cfg->num_subbands)) {
        return false;
    }
    if (!is_one_bit_set_u8(cfg->alloc_mthd) || ((cfg->alloc_mthd & caps->alloc_mthd) != cfg->alloc_mthd)) {
        return false;
    }

    if (cfg->min_bitpool < caps->min_bitpool || cfg->max_bitpool > caps->max_bitpool || cfg->min_bitpool > cfg->max_bitpool) {
        return false;
    }

    return true;
}

static void bt_app_a2d_set_pref_mcc(esp_a2d_conn_hdl_t conn_hdl, const esp_a2d_mcc_t *sink_caps)
{
    esp_a2d_mcc_t pref_mcc;

    memset(&pref_mcc, 0, sizeof(pref_mcc));
    pref_mcc.type = ESP_A2D_MCT_SBC;
    pref_mcc.cie.sbc_info.samp_freq    = ESP_A2D_SBC_CIE_SF_44K;
    pref_mcc.cie.sbc_info.ch_mode      = ESP_A2D_SBC_CIE_CH_MODE_MONO;
    pref_mcc.cie.sbc_info.block_len    = ESP_A2D_SBC_CIE_BLOCK_LEN_16;
    pref_mcc.cie.sbc_info.num_subbands = ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
    pref_mcc.cie.sbc_info.alloc_mthd   = ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
    pref_mcc.cie.sbc_info.min_bitpool  = 2;
    pref_mcc.cie.sbc_info.max_bitpool  = 35;

    if (!check_pref_mcc_against_sink_caps(sink_caps, &pref_mcc)) {
        ESP_LOGE(BT_AV_TAG, "Unsupported headphones: require SBC 44100 Hz mono, 16 blocks, "
                 "8 subbands, loudness, bitpool 2..35. Audio will remain stopped.");
        return;
    }

    esp_err_t ret = esp_a2d_source_set_pref_mcc(conn_hdl, &pref_mcc);
    if (ret != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "Codec configuration request failed: %s; audio will remain stopped", esp_err_to_name(ret));
    }
}

static void bt_app_av_state_unconnected_hdlr(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;
    /* handle the events of interest in unconnected state */
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        break;
    case BT_APP_HEART_BEAT_EVT: {
        uint8_t *bda = s_peer_bda;
        ESP_LOGI(BT_AV_TAG, "a2dp connecting to peer: %02x:%02x:%02x:%02x:%02x:%02x",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        esp_err_t err = esp_a2d_source_connect(s_peer_bda);
        if (err == ESP_OK) {
            s_a2d_state = APP_AV_STATE_CONNECTING;
        } else {
            ESP_LOGE(BT_AV_TAG, "Connection request failed: %s; will retry", esp_err_to_name(err));
        }
        s_connecting_intv = 0;
        break;
    }
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        ESP_LOGI(BT_AV_TAG, "%s, delay value: %u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
        break;
    }
    default: {
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
    }
}

static void bt_app_av_state_connecting_hdlr(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;

    /* handle the events of interest in connecting state */
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(BT_AV_TAG, "a2dp connected");
            s_a2d_state =  APP_AV_STATE_CONNECTED;
            s_media_state = APP_AV_MEDIA_STATE_IDLE;
        } else if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_a2d_state =  APP_AV_STATE_UNCONNECTED;
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        break;
    case BT_APP_HEART_BEAT_EVT:
        /**
         * Switch state to APP_AV_STATE_UNCONNECTED
         * when connecting lasts more than 2 heart beat intervals.
         */
        if (++s_connecting_intv >= 2) {
            s_a2d_state = APP_AV_STATE_UNCONNECTED;
            s_connecting_intv = 0;
        }
        break;
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        ESP_LOGI(BT_AV_TAG, "%s, delay value: %u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
        break;
    }
    default:
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
}

static void bt_app_av_media_proc(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;

    switch (s_media_state) {
    case APP_AV_MEDIA_STATE_IDLE: {
        if (event == BT_APP_HEART_BEAT_EVT) {
            portENTER_CRITICAL(&s_audio_lock);
            bool format_ok = s_pcm_format_confirmed;
            portEXIT_CRITICAL(&s_audio_lock);
            if (!format_ok) {
                break; /* Never start a stream whose PCM packing is unconfirmed. */
            }
            ESP_LOGI(BT_AV_TAG, "a2dp media ready checking ...");
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY));
        } else if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
            a2d = (esp_a2d_cb_param_t *)(param);
            if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
                    a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGI(BT_AV_TAG, "a2dp media ready, starting ...");
                esp_err_t err = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
                if (err == ESP_OK) {
                    s_media_state = APP_AV_MEDIA_STATE_STARTING;
                } else {
                    ESP_LOGE(BT_AV_TAG, "Media start request failed: %s; will retry", esp_err_to_name(err));
                }
            }
        }
        break;
    }
    case APP_AV_MEDIA_STATE_STARTING: {
        if (event == ESP_A2D_MEDIA_CTRL_ACK_EVT) {
            a2d = (esp_a2d_cb_param_t *)(param);
            if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START &&
                    a2d->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGI(BT_AV_TAG, "a2dp media start successfully.");
                s_media_state = APP_AV_MEDIA_STATE_STARTED;
            } else {
                /* not started successfully, transfer to idle state */
                ESP_LOGI(BT_AV_TAG, "a2dp media start failed.");
                s_media_state = APP_AV_MEDIA_STATE_IDLE;
            }
        }
        break;
    }
    case APP_AV_MEDIA_STATE_STARTED: {
        /* Live microphone audio runs continuously. The original demo stopped
         * after ten heartbeats (~100 s); a heartbeat must not stop this stream. */
        break;
    }
    default: {
        break;
    }
    }
}

static void bt_app_av_state_connected_hdlr(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;

    /* handle the events of interest in connected state */
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(BT_AV_TAG, "a2dp disconnected");
            s_a2d_state = APP_AV_STATE_UNCONNECTED;
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        if (ESP_A2D_AUDIO_STATE_STARTED == a2d->audio_stat.state) {
            s_pkt_cnt = 0;
            s_media_state = APP_AV_MEDIA_STATE_STARTED;
        } else {
            /* Continuous listening resumes on the next heartbeat after suspend. */
            s_media_state = APP_AV_MEDIA_STATE_IDLE;
        }
        break;
    }
    case ESP_A2D_AUDIO_CFG_EVT:
        // not supposed to occur for A2DP source
        break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    case BT_APP_HEART_BEAT_EVT: {
        bt_app_av_media_proc(event, param);
        break;
    }
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        ESP_LOGI(BT_AV_TAG, "%s, delay value: %u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
        break;
    }
    case ESP_A2D_REPORT_SNK_CODEC_CAPS_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        esp_a2d_mcc_t *sink_mcc = &a2d->a2d_report_snk_codec_caps_stat.mcc;
        ESP_LOGI(BT_AV_TAG, "sink codec type: %d", sink_mcc->type);
        /* for now only SBC stream is supported */
        if (sink_mcc->type == ESP_A2D_MCT_SBC) {
            ESP_LOGI(BT_AV_TAG, "sink codec capabilities: 0x%x-0x%x-0x%x-0x%x-0x%x-%d-%d",
                     sink_mcc->cie.sbc_info.samp_freq,
                     sink_mcc->cie.sbc_info.ch_mode,
                     sink_mcc->cie.sbc_info.block_len,
                     sink_mcc->cie.sbc_info.num_subbands,
                     sink_mcc->cie.sbc_info.alloc_mthd,
                     sink_mcc->cie.sbc_info.min_bitpool,
                     sink_mcc->cie.sbc_info.max_bitpool);
        }
        bt_app_a2d_set_pref_mcc(a2d->a2d_report_snk_codec_caps_stat.conn_hdl, sink_mcc);
        break;
    }
    case ESP_A2D_SRC_SET_PREF_MCC_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        ESP_LOGI(BT_AV_TAG, "Set preferred media codec config result: conn_hdl: %d, set_status: %d",
                 a2d->a2d_set_pref_mcc_stat.conn_hdl, a2d->a2d_set_pref_mcc_stat.set_status);
        break;
    }
    default: {
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
    }
}

static void bt_app_av_state_disconnecting_hdlr(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = NULL;

    /* handle the events of interest in disconnecing state */
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(BT_AV_TAG, "a2dp disconnected");
            s_a2d_state =  APP_AV_STATE_UNCONNECTED;
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
    case BT_APP_HEART_BEAT_EVT:
        break;
    case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(param);
        ESP_LOGI(BT_AV_TAG, "%s, delay value: 0x%u * 1/10 ms", __func__, a2d->a2d_report_delay_value_stat.delay_value);
        break;
    }
    default: {
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
    }
}

/* callback function for AVRCP controller */
static void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
    case ESP_AVRC_CT_PROF_STATE_EVT: {
        if (!bt_app_work_dispatch(bt_av_hdl_avrc_ct_evt, event, param, sizeof(esp_avrc_ct_cb_param_t), NULL, NULL)) {
            ESP_LOGE(BT_RC_CT_TAG, "Lost AVRCP event %d", event);
        }
        break;
    }
    default: {
        ESP_LOGE(BT_RC_CT_TAG, "Invalid AVRC event: %d", event);
        break;
    }
    }
}

static void bt_av_volume_changed(void)
{
    if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_avrc_peer_rn_cap,
                                           ESP_AVRC_RN_VOLUME_CHANGE)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_avrc_ct_send_register_notification_cmd(
            APP_RC_CT_TL_RN_VOLUME_CHANGE, ESP_AVRC_RN_VOLUME_CHANGE, 0));
    }
}

void bt_av_notify_evt_handler(uint8_t event_id, esp_avrc_rn_param_t *event_parameter)
{
    switch (event_id) {
    /* when volume changed locally on target, this event comes */
    case ESP_AVRC_RN_VOLUME_CHANGE: {
        ESP_LOGI(BT_RC_CT_TAG, "Volume changed: %d", event_parameter->volume);
        /* Observe/re-register only. The example's volume + 5 command fought
         * the listener's controls and could request a value above 127. */
        bt_av_volume_changed();
        break;
    }
    /* other */
    default:
        break;
    }
}

/* AVRC controller event handler */
static void bt_av_hdl_avrc_ct_evt(uint16_t event, void *p_param)
{
    ESP_LOGD(BT_RC_CT_TAG, "%s evt %d", __func__, event);
    esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)(p_param);

    switch (event) {
    /* when connection state changed, this event comes */
    case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
        uint8_t *bda = rc->conn_stat.remote_bda;
        ESP_LOGI(BT_RC_CT_TAG, "AVRC conn_state event: state %d, [%02x:%02x:%02x:%02x:%02x:%02x]",
                 rc->conn_stat.connected, bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);

        if (rc->conn_stat.connected) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_avrc_ct_send_get_rn_capabilities_cmd(APP_RC_CT_TL_GET_CAPS));
        } else {
            s_avrc_peer_rn_cap.bits = 0;
        }
        break;
    }
    /* when passthrough responded, this event comes */
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC passthrough response: key_code 0x%x, key_state %d, rsp_code %d", rc->psth_rsp.key_code,
                    rc->psth_rsp.key_state, rc->psth_rsp.rsp_code);
        break;
    }
    /* when notification changed, this event comes */
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC event notification: %d", rc->change_ntf.event_id);
        bt_av_notify_evt_handler(rc->change_ntf.event_id, &rc->change_ntf.event_parameter);
        break;
    }
    /* when indicate feature of remote device, this event comes */
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "AVRC remote features %"PRIx32", TG features %x", rc->rmt_feats.feat_mask, rc->rmt_feats.tg_feat_flag);
        break;
    }
    /* when get supported notification events capability of peer device, this event comes */
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "remote rn_cap: count %d, bitmask 0x%x", rc->get_rn_caps_rsp.cap_count,
                 rc->get_rn_caps_rsp.evt_set.bits);
        s_avrc_peer_rn_cap.bits = rc->get_rn_caps_rsp.evt_set.bits;

        bt_av_volume_changed();
        break;
    }
    /* when set absolute volume responded, this event comes */
    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT: {
        ESP_LOGI(BT_RC_CT_TAG, "Set absolute volume response: volume %d", rc->set_volume_rsp.volume);
        break;
    }
    /* when avrcp controller init or deinit completed, this event comes */
    case ESP_AVRC_CT_PROF_STATE_EVT: {
        if (ESP_AVRC_INIT_SUCCESS == rc->avrc_ct_init_stat.state) {
            ESP_LOGI(BT_RC_CT_TAG, "AVRCP CT STATE: Init Complete");
        } else if (ESP_AVRC_DEINIT_SUCCESS == rc->avrc_ct_init_stat.state) {
            ESP_LOGI(BT_RC_CT_TAG, "AVRCP CT STATE: Deinit Complete");
        } else {
            ESP_LOGE(BT_RC_CT_TAG, "AVRCP CT STATE error: %d", rc->avrc_ct_init_stat.state);
        }
        break;
    }
    /* other */
    default: {
        ESP_LOGE(BT_RC_CT_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
    }
}

/*********************************
 * MAIN ENTRY POINT
 ********************************/

void app_main(void)
{
    char bda_str[18] = {0};
    /* initialize NVS — it is used to store PHY calibration data */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Pin before the first float operation: ESP-IDF otherwise auto-pins an
     * FPU-using task to whichever core first executes it. Priority 18 keeps
     * capture above the app task (10), below Bluetooth host tasks (19+). */
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(microphone_task, "microphone", 3072, NULL,
                    MIC_TASK_PRIORITY, NULL, MIC_TASK_CORE) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    /*
     * This example only uses the functions of Classical Bluetooth.
     * So release the controller memory for Bluetooth Low Energy.
     */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s initialize controller failed", __func__);
        return;
    }
    if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s enable controller failed", __func__);
        return;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
#if (CONFIG_EXAMPLE_SSP_ENABLED == false)
    bluedroid_cfg.ssp_en = false;
#endif
    if ((ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg)) != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s initialize bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    if (esp_bluedroid_enable() != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "%s enable bluedroid failed", __func__);
        return;
    }

#if (CONFIG_EXAMPLE_SSP_ENABLED == true)
    /* set default parameters for Secure Simple Pairing */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    ESP_ERROR_CHECK(esp_bt_gap_set_security_param(param_type, &iocap, sizeof(iocap)));
#endif

    /*
     * Set default parameters for Legacy Pairing
     * Use variable pin, input pin code when pairing
     */
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code = {0};
    ESP_ERROR_CHECK(esp_bt_gap_set_pin(pin_type, 0, pin_code));

    ESP_LOGI(BT_AV_TAG, "Own address:[%s]", bda2str((uint8_t *)esp_bt_dev_get_address(), bda_str, sizeof(bda_str)));
    bt_app_task_start_up();
    /* Bluetooth device name, connection mode and profile set up */
    ESP_ERROR_CHECK(bt_app_work_dispatch(bt_av_hdl_stack_evt, BT_APP_STACK_UP_EVT, NULL, 0, NULL, NULL)
                    ? ESP_OK : ESP_FAIL);
}

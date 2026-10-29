#include "avrc_media.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AVRC_MEDIA";

#define TITLE_MAX      78
#define ARTIST_MAX     46
#define SEEK_STEP_MS   10000   /* what one FF/REW press usually seeks on Android */

static char s_title[TITLE_MAX + 1];
static char s_artist[ARTIST_MAX + 1];
static volatile uint32_t s_pos_ms;
static volatile uint32_t s_dur_ms;
static volatile bool s_playing;
static volatile bool s_valid;
static volatile bool s_connected;
static volatile int64_t s_pos_tick_us;
static uint8_t s_tl;

static uint8_t next_tl(void)
{
    s_tl = (uint8_t)((s_tl + 1) & 0x0F);
    return s_tl;
}

static void copy_text(char *dst, size_t cap, const uint8_t *src, int len)
{
    if (!dst || cap == 0) return;
    if (!src || len <= 0) {
        dst[0] = '\0';
        return;
    }
    size_t n = (size_t)len < cap - 1 ? (size_t)len : cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';

    /* Some phones pad attributes and send control bytes; make it printable. */
    for (char *p = dst; *p; ++p) {
        if ((unsigned char)*p < 0x20 || *p == 0x7F) *p = ' ';
    }
    size_t l = strlen(dst);
    while (l > 0 && (dst[l - 1] == ' ' || dst[l - 1] == '\n' || dst[l - 1] == '\r')) {
        dst[--l] = '\0';
    }
}

static void register_notifications(void)
{
    esp_avrc_ct_send_register_notification_cmd(next_tl(), ESP_AVRC_RN_TRACK_CHANGE, 0);
    esp_avrc_ct_send_register_notification_cmd(next_tl(), ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
    esp_avrc_ct_send_register_notification_cmd(next_tl(), ESP_AVRC_RN_PLAY_POS_CHANGED, 1);
}

void avrc_media_request_state(void)
{
    if (!s_connected) return;
    esp_avrc_ct_send_metadata_cmd(
        next_tl(),
        ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_PLAYING_TIME
    );
    esp_avrc_ct_send_get_play_status_cmd(next_tl());
}

bool avrc_media_connected(void)
{
    return s_connected;
}

void avrc_media_on_event(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        s_connected = param->conn_stat.connected;
        if (s_connected) {
            s_title[0] = '\0';
            s_artist[0] = '\0';
            s_pos_ms = 0;
            s_dur_ms = 0;
            s_playing = false;
            s_valid = false;
            s_pos_tick_us = esp_timer_get_time();
            register_notifications();
            avrc_media_request_state();
            ESP_LOGI(TAG, "AVRCP connected, metadata subscriptions sent");
        } else {
            s_playing = false;
            s_valid = false;
            ESP_LOGI(TAG, "AVRCP disconnected");
        }
        break;

    case ESP_AVRC_CT_METADATA_RSP_EVT: {
        const uint8_t attr = param->meta_rsp.attr_id;
        const uint8_t *txt = param->meta_rsp.attr_text;
        const int len = param->meta_rsp.attr_length;

        if (attr == ESP_AVRC_MD_ATTR_TITLE) {
            copy_text(s_title, sizeof(s_title), txt, len);
            ESP_LOGI(TAG, "Title: %s", s_title);
        } else if (attr == ESP_AVRC_MD_ATTR_ARTIST) {
            copy_text(s_artist, sizeof(s_artist), txt, len);
            ESP_LOGI(TAG, "Artist: %s", s_artist);
        } else if (attr == ESP_AVRC_MD_ATTR_PLAYING_TIME) {
            /* arrives as a decimal string in milliseconds */
            char tmp[16] = {0};
            copy_text(tmp, sizeof(tmp), txt, len);
            uint32_t ms = (uint32_t)strtoul(tmp, NULL, 10);
            if (ms > 0) s_dur_ms = ms;
        }
        s_valid = true;
        break;
    }

    case ESP_AVRC_CT_PLAY_STATUS_RSP_EVT:
        s_dur_ms = param->play_status_rsp.song_length;
        s_pos_ms = param->play_status_rsp.song_position;
        s_pos_tick_us = esp_timer_get_time();
        s_playing = param->play_status_rsp.play_status == ESP_AVRC_PLAYBACK_PLAYING;
        s_valid = true;
        break;

    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT: {
        const uint8_t ev = param->change_ntf.event_id;
        if (ev == ESP_AVRC_RN_TRACK_CHANGE) {
            avrc_media_request_state();
            esp_avrc_ct_send_register_notification_cmd(next_tl(), ESP_AVRC_RN_TRACK_CHANGE, 0);
        } else if (ev == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
            s_playing = param->change_ntf.event_parameter.playback == ESP_AVRC_PLAYBACK_PLAYING;
            s_pos_tick_us = esp_timer_get_time();
            s_valid = true;
            esp_avrc_ct_send_get_play_status_cmd(next_tl());
            esp_avrc_ct_send_register_notification_cmd(next_tl(), ESP_AVRC_RN_PLAY_STATUS_CHANGE, 0);
        } else if (ev == ESP_AVRC_RN_PLAY_POS_CHANGED) {
            s_pos_ms = param->change_ntf.event_parameter.play_pos;
            s_pos_tick_us = esp_timer_get_time();
            s_valid = true;
            esp_avrc_ct_send_register_notification_cmd(next_tl(), ESP_AVRC_RN_PLAY_POS_CHANGED, 1);
        }
        break;
    }

    default:
        break;
    }
}

static uint32_t current_position_ms(void)
{
    uint32_t pos = s_pos_ms;
    if (s_playing) {
        int64_t dt_us = esp_timer_get_time() - s_pos_tick_us;
        if (dt_us > 0) pos += (uint32_t)(dt_us / 1000);
    }
    if (s_dur_ms > 0 && pos > s_dur_ms) pos = s_dur_ms;
    return pos;
}

size_t avrc_media_build_payload(uint8_t *buf, size_t cap)
{
    if (!buf || cap < 11) return 0;

    size_t tlen = strlen(s_title);
    if (tlen > TITLE_MAX) tlen = TITLE_MAX;
    size_t alen = strlen(s_artist);
    if (alen > ARTIST_MAX) alen = ARTIST_MAX;

    size_t need = 1 + 4 + 4 + 1 + tlen + 1 + alen;
    if (need > cap) return 0;

    const uint32_t pos = current_position_ms();
    uint8_t flags = 0;
    if (s_valid) flags |= 0x01;
    if (s_playing) flags |= 0x02;

    size_t i = 0;
    buf[i++] = flags;
    buf[i++] = (uint8_t)(pos & 0xFF);
    buf[i++] = (uint8_t)((pos >> 8) & 0xFF);
    buf[i++] = (uint8_t)((pos >> 16) & 0xFF);
    buf[i++] = (uint8_t)((pos >> 24) & 0xFF);
    buf[i++] = (uint8_t)(s_dur_ms & 0xFF);
    buf[i++] = (uint8_t)((s_dur_ms >> 8) & 0xFF);
    buf[i++] = (uint8_t)((s_dur_ms >> 16) & 0xFF);
    buf[i++] = (uint8_t)((s_dur_ms >> 24) & 0xFF);
    buf[i++] = (uint8_t)tlen;
    memcpy(&buf[i], s_title, tlen);
    i += tlen;
    buf[i++] = (uint8_t)alen;
    memcpy(&buf[i], s_artist, alen);
    i += alen;
    return i;
}

void avrc_media_seek(int32_t delta_ms)
{
    if (!s_connected || delta_ms == 0) return;
    /* Ignore tiny changes: a single FF/REW press already moves ~10 s. */
    if (delta_ms > -5000 && delta_ms < 5000) {
        ESP_LOGI(TAG, "Seek %+ld ms ignored (too small)", (long)delta_ms);
        return;
    }

    int steps = (int)((delta_ms < 0 ? -delta_ms : delta_ms) / SEEK_STEP_MS);
    if (steps < 1) steps = 1;
    /* Long bursts are turned into "next track" by some players (YouTube),
       so never send more than three presses. */
    if (steps > 3) steps = 3;

    const uint8_t key = delta_ms > 0 ? ESP_AVRC_PT_CMD_FORWARD : ESP_AVRC_PT_CMD_BACKWARD;
    ESP_LOGI(TAG, "Seek %+ld ms -> %d x %s", (long)delta_ms, steps,
             delta_ms > 0 ? "FF" : "REW");

    for (int i = 0; i < steps; i++) {
        esp_avrc_ct_send_passthrough_cmd(next_tl(), key, ESP_AVRC_PT_CMD_STATE_PRESSED);
        vTaskDelay(pdMS_TO_TICKS(60));
        esp_avrc_ct_send_passthrough_cmd(next_tl(), key, ESP_AVRC_PT_CMD_STATE_RELEASED);
        vTaskDelay(pdMS_TO_TICKS(450));
    }
    /* The phone needs a moment to update; refresh right after. */
    vTaskDelay(pdMS_TO_TICKS(400));
    avrc_media_request_state();
}

static void avrc_media_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    while (true) {
        if (s_connected) {
            /* Position is not pushed continuously, so poll it. */
            esp_avrc_ct_send_get_play_status_cmd(next_tl());
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

void avrc_media_start(void)
{
    xTaskCreate(avrc_media_task, "avrc_media", 3072, NULL, 3, NULL);
}

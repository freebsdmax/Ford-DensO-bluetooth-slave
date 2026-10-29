#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_avrc_api.h"

/**
 * AVRCP metadata collection for the Bluetooth sink (external/BT mode).
 *
 * The phone is the AVRCP Target (it owns the media), we are the Controller:
 * we subscribe to TRACK_CHANGE / PLAY_STATUS_CHANGE / PLAY_POS_CHANGED, ask
 * for TITLE / ARTIST / PLAYING_TIME and poll GetPlayStatus for the position.
 */

/** Called from the AVRCP CT callback (a2dp-slave.c). */
void avrc_media_on_event(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);

/** Starts the background task that polls the playback position. */
void avrc_media_start(void);

/** Re-requests metadata + play status (used when the app asks for a refresh). */
void avrc_media_request_state(void);

bool avrc_media_connected(void);

/**
 * Serialises the current state for the UART frame:
 * [flags:1][pos_ms:4][dur_ms:4][title_len:1][title][artist_len:1][artist]
 * flags: bit0 = valid, bit1 = playing
 * @return payload length
 */
size_t avrc_media_build_payload(uint8_t *buf, size_t cap);

/** Approximate seek: AVRCP has no absolute seek, so FF/REW presses are used. */
void avrc_media_seek(int32_t delta_ms);

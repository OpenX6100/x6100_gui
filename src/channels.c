/*
 * Channel database
 *
 * Copyright (C) 2026 Hany El Imam
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "channels.h"
#include "cJSON.h"
#include "cfg/cfg_api.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>


#define CHANNELS_FILE "/mnt/channels.json"
#define CHANNELS_VERSION 1


static channel_t channels[CHANNELS_MAX];
static uint16_t channel_count = 0;


/*
 * Convert X6100 mode enum to readable JSON string.
 */
static const char *mode_to_string(x6100_mode_t mode) {

    switch (mode) {

        case x6100_mode_lsb:
            return "LSB";

        case x6100_mode_usb:
            return "USB";

        case x6100_mode_lsb_dig:
            return "LSB-D";

        case x6100_mode_usb_dig:
            return "USB-D";

        case x6100_mode_cw:
            return "CW";

        case x6100_mode_cwr:
            return "CWR";

        case x6100_mode_am:
            return "AM";

        case x6100_mode_nfm:
            return "NFM";

        default:
            return "USB";
    }
}


/*
 * Convert readable JSON mode string to X6100 enum.
 */
static bool string_to_mode(
    const char *value,
    x6100_mode_t *mode
) {

    if (!value || !mode) {
        return false;
    }


    if (strcmp(value, "LSB") == 0) {
        *mode = x6100_mode_lsb;
        return true;
    }

    if (strcmp(value, "USB") == 0) {
        *mode = x6100_mode_usb;
        return true;
    }

    if (strcmp(value, "LSB-D") == 0) {
        *mode = x6100_mode_lsb_dig;
        return true;
    }

    if (strcmp(value, "USB-D") == 0) {
        *mode = x6100_mode_usb_dig;
        return true;
    }

    if (strcmp(value, "CW") == 0) {
        *mode = x6100_mode_cw;
        return true;
    }

    if (strcmp(value, "CWR") == 0) {
        *mode = x6100_mode_cwr;
        return true;
    }

    if (strcmp(value, "AM") == 0) {
        *mode = x6100_mode_am;
        return true;
    }

    if (strcmp(value, "NFM") == 0) {
        *mode = x6100_mode_nfm;
        return true;
    }


    return false;
}


/*
 * Convert AGC enum to readable JSON string.
 */
static const char *agc_to_string(x6100_agc_t agc) {

    switch (agc) {

        case x6100_agc_off:
            return "OFF";

        case x6100_agc_slow:
            return "SLOW";

        case x6100_agc_fast:
            return "FAST";

        case x6100_agc_auto:
            return "AUTO";

        default:
            return "AUTO";
    }
}


/*
 * Convert readable JSON AGC string to X6100 enum.
 */
static bool string_to_agc(
    const char *value,
    x6100_agc_t *agc
) {

    if (!value || !agc) {
        return false;
    }


    if (strcmp(value, "OFF") == 0) {
        *agc = x6100_agc_off;
        return true;
    }

    if (strcmp(value, "SLOW") == 0) {
        *agc = x6100_agc_slow;
        return true;
    }

    if (strcmp(value, "FAST") == 0) {
        *agc = x6100_agc_fast;
        return true;
    }

    if (strcmp(value, "AUTO") == 0) {
        *agc = x6100_agc_auto;
        return true;
    }


    return false;
}


/*
 * Save current in-memory database to
 * /mnt/channels.json.
 */
bool channels_save(void) {

    bool result = false;

    cJSON *root = NULL;
    cJSON *array = NULL;
    char *json_text = NULL;
    FILE *file = NULL;


    root = cJSON_CreateObject();

    if (!root) {
        goto cleanup;
    }


    cJSON_AddNumberToObject(
        root,
        "version",
        CHANNELS_VERSION
    );


    array = cJSON_AddArrayToObject(
        root,
        "channels"
    );

    if (!array) {
        goto cleanup;
    }


    for (uint16_t i = 0; i < channel_count; i++) {

        const channel_t *channel = &channels[i];

        cJSON *item = cJSON_CreateObject();

        if (!item) {
            goto cleanup;
        }


        cJSON_AddStringToObject(
            item,
            "name",
            channel->name
        );

        cJSON_AddNumberToObject(
            item,
            "frequency",
            channel->freq
        );

        cJSON_AddStringToObject(
            item,
            "mode",
            mode_to_string(channel->mode)
        );

        cJSON_AddStringToObject(
            item,
            "agc",
            agc_to_string(channel->agc)
        );

        cJSON_AddBoolToObject(
            item,
            "pre",
            channel->pre
        );

        cJSON_AddBoolToObject(
            item,
            "att",
            channel->att
        );


        cJSON_AddItemToArray(
            array,
            item
        );
    }


    /*
     * Pretty printed JSON:
     * intentionally human-readable/editable.
     */
    json_text = cJSON_Print(root);

    if (!json_text) {
        goto cleanup;
    }


    /* Written to a .tmp file and renamed over channels.json, so a power
     * cut while saving never leaves a half-written list. */
    file = fopen(
        CHANNELS_FILE ".tmp",
        "w"
    );

    if (!file) {
        goto cleanup;
    }


    if (fputs(json_text, file) == EOF) {
        goto cleanup;
    }


    /*
     * Add final newline for nicer manual editing.
     */
    if (fputc('\n', file) == EOF) {
        goto cleanup;
    }

    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
        goto cleanup;
    }

    if (fclose(file) != 0) {
        file = NULL;
        goto cleanup;
    }
    file = NULL;

    if (rename(CHANNELS_FILE ".tmp", CHANNELS_FILE) != 0) {
        goto cleanup;
    }

    result = true;


cleanup:

    if (file) {
        fclose(file);
    }

    if (!result) {
        unlink(CHANNELS_FILE ".tmp");
    }

    if (json_text) {
        cJSON_free(json_text);
    }

    if (root) {
        cJSON_Delete(root);
    }

    return result;
}


/*
 * Read entire JSON file into memory.
 */
static char *read_json_file(void) {

    FILE *file = fopen(
        CHANNELS_FILE,
        "rb"
    );

    if (!file) {
        return NULL;
    }


    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }


    long size = ftell(file);

    if (size < 0) {
        fclose(file);
        return NULL;
    }


    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }


    char *buffer = malloc(
        (size_t)size + 1
    );

    if (!buffer) {
        fclose(file);
        return NULL;
    }


    size_t read_size = fread(
        buffer,
        1,
        (size_t)size,
        file
    );

    fclose(file);


    if (read_size != (size_t)size) {
        free(buffer);
        return NULL;
    }


    buffer[size] = '\0';

    return buffer;
}


/*
 * Load channel database.
 *
 * If /mnt/channels.json does not exist:
 *
 *   the list starts empty and an empty channels.json is created
 *
 * If it exists:
 *
 *   load channels from JSON.
 */
bool channels_load(void) {

    FILE *test_file = fopen(
        CHANNELS_FILE,
        "rb"
    );


    if (!test_file) {

        channel_count = 0;

        return channels_save();
    }


    fclose(test_file);


    char *json_text = read_json_file();

    if (!json_text) {
        return false;
    }


    cJSON *root = cJSON_Parse(
        json_text
    );

    free(json_text);


    if (!root) {
        return false;
    }


    cJSON *array = cJSON_GetObjectItemCaseSensitive(
        root,
        "channels"
    );


    if (!cJSON_IsArray(array)) {
        cJSON_Delete(root);
        return false;
    }


    channel_count = 0;


    cJSON *item = NULL;

    cJSON_ArrayForEach(item, array) {

        if (channel_count >= CHANNELS_MAX) {
            break;
        }


        if (!cJSON_IsObject(item)) {
            continue;
        }


        cJSON *name = cJSON_GetObjectItemCaseSensitive(
            item,
            "name"
        );

        cJSON *frequency = cJSON_GetObjectItemCaseSensitive(
            item,
            "frequency"
        );

        cJSON *mode = cJSON_GetObjectItemCaseSensitive(
            item,
            "mode"
        );

        cJSON *agc = cJSON_GetObjectItemCaseSensitive(
            item,
            "agc"
        );

        cJSON *pre = cJSON_GetObjectItemCaseSensitive(
            item,
            "pre"
        );

        cJSON *att = cJSON_GetObjectItemCaseSensitive(
            item,
            "att"
        );


        if (!cJSON_IsString(name) ||
            !name->valuestring ||
            !cJSON_IsNumber(frequency) ||
            !cJSON_IsString(mode) ||
            !mode->valuestring ||
            !cJSON_IsString(agc) ||
            !agc->valuestring ||
            !cJSON_IsBool(pre) ||
            !cJSON_IsBool(att)) {

            continue;
        }


        if (frequency->valuedouble <= 0 ||
            frequency->valuedouble > UINT32_MAX) {

            continue;
        }


        x6100_mode_t parsed_mode;
        x6100_agc_t parsed_agc;


        if (!string_to_mode(
                mode->valuestring,
                &parsed_mode
            )) {

            continue;
        }


        if (!string_to_agc(
                agc->valuestring,
                &parsed_agc
            )) {

            continue;
        }


        channel_t *channel =
            &channels[channel_count];


        strncpy(
            channel->name,
            name->valuestring,
            CHANNEL_NAME_MAX - 1
        );

        channel->name[
            CHANNEL_NAME_MAX - 1
        ] = '\0';


        channel->freq =
            (uint32_t)frequency->valuedouble;


        channel->mode = parsed_mode;
        channel->agc = parsed_agc;

        channel->pre =
            cJSON_IsTrue(pre);

        channel->att =
            cJSON_IsTrue(att);


        channel_count++;
    }


    cJSON_Delete(root);

    return true;
}


/*
 * Return number of loaded channels.
 */
uint16_t channels_count(void) {

    return channel_count;
}


/*
 * Return channel at index.
 */
const channel_t *channels_get(
    uint16_t index
) {

    if (index >= channel_count) {
        return NULL;
    }

    return &channels[index];
}


/*
 * Add current radio state as a new channel.
 *
 * The new channel is always appended at the end.
 * Its name is initially empty.
 */
bool channels_add_current(void) {

    if (channel_count >= CHANNELS_MAX) {
        return false;
    }


    channel_t *channel =
        &channels[channel_count];


    memset(
        channel,
        0,
        sizeof(channel_t)
    );


    channel->name[0] = '\0';


    channel->freq =
        (uint32_t)cparam_i_get(cfg.cur.fg_freq());


    channel->mode =
        (x6100_mode_t)cparam_i_get(cfg.cur.mode());


    channel->agc =
        (x6100_agc_t)cparam_i_get(cfg.cur.agc());


    channel->pre =
        cparam_i_get(cfg.cur.pre()) != 0;


    channel->att =
        cparam_i_get(cfg.cur.att()) != 0;


    channel_count++;


    if (!channels_save()) {

        channel_count--;

        memset(
            &channels[channel_count],
            0,
            sizeof(channel_t)
        );

        return false;
    }


    return true;
}


/*
 * Delete a channel from the database.
 *
 * All following channels are shifted up
 * by one position, so no empty record
 * remains in the list.
 */
bool channels_delete(uint16_t index) {

    if (index >= channel_count) {
        return false;
    }


    channel_t backup[CHANNELS_MAX];
    uint16_t old_count = channel_count;

    memcpy(
        backup,
        channels,
        sizeof(channels)
    );


    for (uint16_t i = index;
         i + 1 < channel_count;
         i++) {

        channels[i] =
            channels[i + 1];
    }


    channel_count--;

    memset(
        &channels[channel_count],
        0,
        sizeof(channel_t)
    );


    if (!channels_save()) {

        memcpy(
            channels,
            backup,
            sizeof(channels)
        );

        channel_count = old_count;

        return false;
    }


    return true;
}


/*
 * Change the name/label of an existing channel.
 */
bool channels_set_name(uint16_t index, const char *name) {

    if (index >= channel_count || !name) {
        return false;
    }


    char old_name[CHANNEL_NAME_MAX];

    snprintf(
        old_name,
        sizeof(old_name),
        "%s",
        channels[index].name
    );


    snprintf(
        channels[index].name,
        sizeof(channels[index].name),
        "%s",
        name
    );


    if (!channels_save()) {

        snprintf(
            channels[index].name,
            sizeof(channels[index].name),
            "%s",
            old_name
        );

        return false;
    }


    return true;
}


/*
 * Move a channel inside the in-memory database.
 *
 * No JSON write is performed here.
 *
 * This is intentionally separate from channels_save()
 * because the Drag operation can move a record several
 * times while the MFK encoder is being rotated.
 *
 * The final order will be written to disk only when
 * the user presses Drop.
 */
bool channels_move(
    uint16_t from,
    uint16_t to
) {

    /*
     * Both positions must exist.
     */
    if (from >= channel_count ||
        to >= channel_count) {

        return false;
    }


    /*
     * Nothing to do.
     */
    if (from == to) {
        return true;
    }


    /*
     * Keep a copy of the channel being moved.
     */
    channel_t moving =
        channels[from];


    /*
     * Moving DOWN:
     *
     *   A B C D
     *     |
     *     +----> after D
     *
     * becomes:
     *
     *   A C D B
     */
    if (from < to) {

        for (uint16_t i = from;
             i < to;
             i++) {

            channels[i] =
                channels[i + 1];
        }
    }


    /*
     * Moving UP:
     *
     *   A B C D
     *       |
     *       +----> before A
     *
     * becomes:
     *
     *   C A B D
     */
    else {

        for (uint16_t i = from;
             i > to;
             i--) {

            channels[i] =
                channels[i - 1];
        }
    }


    /*
     * Insert channel into its new position.
     */
    channels[to] =
        moving;


    return true;
}

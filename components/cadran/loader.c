#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "cadran_internal.h"

static const char *TAG = "cadran.loader";

struct cadran_face {
    uint8_t widget_count;
    uint16_t flags;
    const cadran_widget_rec_t *widgets;
    const char *strings;
    size_t strings_len;
    uint8_t *owned; /* backing allocation; widgets/strings point into it */
};

esp_err_t cadran_face_load(const uint8_t *data, size_t len, cadran_face_t **out_face) {
    if (!data || !out_face) return ESP_ERR_INVALID_ARG;
    if (len < sizeof(cadran_header_t)) {
        ESP_LOGE(TAG, "too short for a header: %u bytes", (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }

    cadran_header_t hdr;
    memcpy(&hdr, data, sizeof hdr);
    if (memcmp(hdr.magic, "CDRN", 4) != 0) {
        ESP_LOGE(TAG, "bad magic");
        return ESP_ERR_INVALID_ARG;
    }
    if (hdr.abi != CADRAN_ABI) {
        ESP_LOGE(TAG, "abi mismatch: face=%d fw=%d", hdr.abi, CADRAN_ABI);
        return ESP_ERR_INVALID_VERSION;
    }

    /* Widget table is always the last section, so its start (and thus the
     * string table's length) follows from widget_count + total length -
     * see face_format.h's header comment. */
    size_t widgets_size = (size_t)hdr.widget_count * sizeof(cadran_widget_rec_t);
    size_t fixed = sizeof(cadran_header_t) + widgets_size;
    if (len < fixed) {
        ESP_LOGE(TAG, "truncated: len=%u need>=%u", (unsigned)len, (unsigned)fixed);
        return ESP_ERR_INVALID_SIZE;
    }
    size_t strings_len = len - fixed;

    const uint8_t *strings_ptr = data + sizeof(cadran_header_t);
    const uint8_t *widgets_ptr = strings_ptr + strings_len;

    /* Validate every str_ref up front so the render path never has to
     * bounds-check strings itself (doc §6: "still bounds-checked
     * defensively; it lives on writable flash"). */
    for (uint8_t i = 0; i < hdr.widget_count; i++) {
        cadran_widget_rec_t w;
        memcpy(&w, widgets_ptr + (size_t)i * sizeof w, sizeof w);
        if (w.str_ref != CADRAN_STR_NONE && w.str_ref >= strings_len) {
            ESP_LOGE(TAG, "widget %d: str_ref %u out of range (strings_len=%u)",
                     i, w.str_ref, (unsigned)strings_len);
            return ESP_ERR_INVALID_ARG;
        }
    }

    cadran_face_t *face = calloc(1, sizeof *face);
    if (!face) return ESP_ERR_NO_MEM;
    uint8_t *owned = malloc(len);
    if (!owned) {
        free(face);
        return ESP_ERR_NO_MEM;
    }
    memcpy(owned, data, len);

    face->widget_count = hdr.widget_count;
    face->flags        = hdr.flags;
    face->strings       = (const char *)(owned + sizeof(cadran_header_t));
    face->strings_len  = strings_len;
    face->widgets       = (const cadran_widget_rec_t *)(owned + sizeof(cadran_header_t) + strings_len);
    face->owned         = owned;

    *out_face = face;
    return ESP_OK;
}

void cadran_face_free(cadran_face_t *face) {
    if (!face) return;
    free(face->owned);
    free(face);
}

uint8_t cadran_face_widget_count(const cadran_face_t *face) {
    return face ? face->widget_count : 0;
}

const cadran_widget_rec_t *cadran_face_widgets(const cadran_face_t *face) {
    return face ? face->widgets : NULL;
}

const char *cadran_face_string(const cadran_face_t *face, uint16_t str_ref) {
    if (!face || str_ref == CADRAN_STR_NONE || str_ref >= face->strings_len) return NULL;
    return face->strings + str_ref;
}

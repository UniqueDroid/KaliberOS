#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "core/event_bus.h"

static QueueHandle_t s_q;

esp_err_t kb_bus_init(size_t queue_len) {
    s_q = xQueueCreate(queue_len, sizeof(event_t));
    return s_q ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t kb_bus_post(const event_t *ev) {
    return xQueueSend(s_q, ev, pdMS_TO_TICKS(50)) == pdTRUE
               ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t kb_bus_post_from_isr(const event_t *ev, bool *hp_task_woken) {
    BaseType_t hpw = pdFALSE;
    BaseType_t ok = xQueueSendFromISR(s_q, ev, &hpw);
    if (hp_task_woken) *hp_task_woken = (hpw == pdTRUE);
    return ok == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

bool kb_bus_receive(event_t *out, uint32_t timeout_ms) {
    return xQueueReceive(s_q, out,
        timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms))
        == pdTRUE;
}

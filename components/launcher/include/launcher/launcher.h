#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the js_task (the single engine-owning task) and runs the app
 * lifecycle. Call after board/bus/store/power init. */
esp_err_t kb_launcher_start(void);

#ifdef __cplusplus
}
#endif

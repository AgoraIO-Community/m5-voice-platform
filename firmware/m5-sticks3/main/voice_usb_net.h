#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t voice_usb_net_start(void);
esp_netif_t *voice_usb_netif(void);
bool voice_usb_net_ready(void);
uint32_t voice_usb_net_generation(void);

#ifdef __cplusplus
}
#endif

#include "voice_usb_net.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"

static esp_netif_t *usb_netif;
static TaskHandle_t link_task;
static atomic_uint carrier_generation;
static atomic_uint network_generation;
static atomic_uint checked_carrier_generation;
static atomic_bool ip_ready;
static atomic_bool initialized;

static esp_err_t transmit(void *ctx, void *frame, size_t len)
{
    if (len > UINT16_MAX) return ESP_ERR_INVALID_SIZE;
    return tinyusb_net_send_sync(frame, (uint16_t)len, NULL, pdMS_TO_TICKS(25));
}

static void release_frame(void *ctx, void *frame)
{
    free(frame);
}

static esp_err_t receive(void *frame, uint16_t len, void *ctx)
{
    // TinyUSB reuses this buffer as soon as its callback returns.
    void *copy = malloc(len);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, frame, len);
    return esp_netif_receive(ctx, copy, len, NULL);
}

static void device_event(tinyusb_event_t *event, void *ctx)
{
    if (event->id == TINYUSB_EVENT_DETACHED) {
        atomic_fetch_add(&carrier_generation, 1);
        atomic_fetch_add(&network_generation, 1);
    }
    xTaskNotifyGive(link_task);
}

static void monitor_link(void *ctx)
{
    bool was_up = false;
    unsigned previous_epoch = 0;
    esp_netif_ip_info_t previous = {0};
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        if (!atomic_load(&initialized)) continue;
        unsigned epoch = atomic_load(&carrier_generation);
        bool up = tud_ready();
        if (was_up && (!up || epoch != previous_epoch)) {
            esp_netif_action_disconnected(ctx, NULL, 0, NULL);
            was_up = false;
        }
        if (up && !was_up) esp_netif_action_connected(ctx, NULL, 0, NULL);
        was_up = up;
        previous_epoch = epoch;

        esp_netif_ip_info_t info = {0};
        bool ready = up && esp_netif_get_ip_info(ctx, &info) == ESP_OK && info.ip.addr;
        if (atomic_exchange(&ip_ready, ready) != ready ||
            (ready && memcmp(&previous, &info, sizeof(info)) != 0)) {
            atomic_fetch_add(&network_generation, 1);
        }
        atomic_store(&checked_carrier_generation, epoch);
        previous = info;
    }
}

esp_err_t voice_usb_net_start(void)
{
    if (usb_netif) return ESP_ERR_INVALID_STATE;
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.if_key = "USB_NCM";
    base.if_desc = "usb";
    // Wi-Fi remains preferred until the route manager verifies the wired path.
    base.route_prio = 5;
    uint8_t device_mac[6];
    esp_err_t err = esp_read_mac(device_mac, ESP_MAC_ETH);
    if (err != ESP_OK) return err;
    memcpy(base.mac, device_mac, sizeof(device_mac));
    esp_netif_driver_ifconfig_t driver = {
        .handle = &usb_netif,
        .transmit = transmit,
        .driver_free_rx_buffer = release_frame,
    };
    esp_netif_config_t cfg = {
        .base = &base, .driver = &driver, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    usb_netif = esp_netif_new(&cfg);
    if (!usb_netif) return ESP_ERR_NO_MEM;
    esp_netif_action_start(usb_netif, NULL, 0, NULL);
    if (xTaskCreate(monitor_link, "usb_link", 3072, usb_netif, 4, &link_task) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    tinyusb_config_t usb = TINYUSB_DEFAULT_CONFIG();
    usb.event_cb = device_event;
    err = tinyusb_driver_install(&usb);
    if (err != ESP_OK) goto fail;

    tinyusb_net_config_t net = {.on_recv_callback = receive, .user_context = usb_netif};
    // iMACAddress is assigned to the host's interface. Give each end its own MAC.
    memcpy(net.mac_addr, device_mac, sizeof(device_mac));
    net.mac_addr[0] |= 0x02;
    net.mac_addr[5] ^= 0x80;
    err = tinyusb_net_init(&net);
    if (err != ESP_OK) {
        tinyusb_driver_uninstall();
        goto fail;
    }
    atomic_store(&initialized, true);
    xTaskNotifyGive(link_task);
    return ESP_OK;

fail:
    if (link_task) {
        vTaskDelete(link_task);
        link_task = NULL;
    }
    esp_netif_action_stop(usb_netif, NULL, 0, NULL);
    esp_netif_destroy(usb_netif);
    usb_netif = NULL;
    return err;
}

esp_netif_t *voice_usb_netif(void)
{
    return atomic_load(&initialized) ? usb_netif : NULL;
}

bool voice_usb_net_ready(void)
{
    return atomic_load(&ip_ready) &&
           atomic_load(&checked_carrier_generation) == atomic_load(&carrier_generation);
}

uint32_t voice_usb_net_generation(void)
{
    return atomic_load(&network_generation);
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Espressif Systems Wireless LAN device driver
 *
 * Custom data interface (ESP_CUSTOM_IF) - Linux host side.
 *
 * Adds an independent channel on top of the esp-hosted SDIO data plane that
 * is fully parallel to WiFi (STA/AP) and BT (HCI). User space reaches it
 * through /dev/esp_custom:
 *
 *   write()  Linux -> ESP32   one write() == one frame
 *   read()   ESP32 -> Linux   one read()  == one frame (frame boundaries kept)
 *   poll()   POLLIN when a frame is available
 *
 * The channel is deliberately frame-oriented rather than a byte stream so the
 * ESP side never has to reassemble.
 */
#ifndef __esp_custom_h_
#define __esp_custom_h_

#include "esp.h"

/* /dev/<name> */
#define ESP_CUSTOM_DEV_NAME     "esp_custom"

/*
 * Largest payload a single write() may carry.
 *
 * The SDIO transport caps a whole frame (payload + esp_payload_header) at
 * ESP_RX_BUFFER_SIZE - sizeof(header) = 15860 bytes, and the ESP firmware
 * aggregates RX frames into a fixed RX_BUF_SIZE buffer, so staying at a
 * conventional MTU keeps every frame on the simple non-fragmented path.
 * At the intended 10-20 KB/s this is never the bottleneck.
 */
#define ESP_CUSTOM_MAX_FRAME    1500

/*
 * How many received frames may sit in the RX queue before new ones are
 * dropped. Only grows while /dev/esp_custom is open - see esp_custom_rx().
 */
#define ESP_CUSTOM_QUEUE_DEPTH  64

int esp_custom_init(void);
void esp_custom_deinit(void);

/*
 * Hand a frame received from the ESP to user space.
 *
 * Called from process_rx_packet() after the skb has already been pulled past
 * the esp_payload_header, i.e. skb->data/skb->len are exactly the payload.
 * Takes ownership of skb in every case - the caller must not free it.
 */
void esp_custom_rx(struct sk_buff *skb);

#endif /* __esp_custom_h_ */

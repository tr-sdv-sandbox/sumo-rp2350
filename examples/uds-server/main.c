/**
 * @file examples/uds-server/main.c
 * @brief Checkpoint-4a: UDS-on-CAN bring-up on RP2350.
 *
 * MVP service set:
 *   0x10  DiagSessionControl   (default / programming / extended)
 *   0x11  ECUReset             (hardReset → watchdog_reboot)
 *   0x22  ReadDID              (0xF187, 0xF195, 0xF18C)
 *   0x3E  TesterPresent        (with suppress-positive-response)
 *
 * Architecture:
 *   - Single-core main loop.
 *   - Core 0: CAN polling → ISO-TP reassembly → UDS dispatch → ISO-TP
 *     transmit. uds_server_poll() handles session/security timers.
 *   - LED pulses to give a lifesign even with no console attached.
 *
 * No SUIT integration yet — that lands in checkpoint 4b. This stage
 * just proves the CAN + ISO-TP + UDS stack works end-to-end against a
 * Linux host running examples/uds-server/host/tester.py.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include "uds/uds_server.h"
#include "uds/uds_session.h"
#include "isotp/isotp.h"
#include "uds_tiny/can_hw.h"
#include "store/did_store.h"

#include "app_config.h"
#include "pin_config.h"

/* ── Diag-LED ─────────────────────────────────────────────────────── */
#ifndef DIAG_LED_PIN
#  ifdef PICO_DEFAULT_LED_PIN
#    define DIAG_LED_PIN PICO_DEFAULT_LED_PIN
#  else
#    define DIAG_LED_PIN 25
#  endif
#endif

static void led_init(void) { gpio_init(DIAG_LED_PIN); gpio_set_dir(DIAG_LED_PIN, GPIO_OUT); }
static void led_set(int on) { gpio_put(DIAG_LED_PIN, on ? 1 : 0); }
static void blink_n(int n, int on_ms, int off_ms) {
    for (int i = 0; i < n; i++) {
        led_set(1); sleep_ms(on_ms);
        led_set(0); sleep_ms(off_ms);
    }
}

#define STAGE(n, msg) do {                              \
    printf("stage %d: %s\n", (n), (msg));               \
    fflush(stdout);                                     \
    sleep_ms(50);                                       \
    blink_n((n), 80, 80);                               \
    sleep_ms(200);                                      \
} while (0)

/* ── ISO-TP CAN-ID pair (29-bit, normal-fixed addressing) ────────── */

#define ISOTP_RX_ID (0x18DA0000U \
                     | ((uint32_t)CAN_ECU_ADDR_DEFAULT << 8) \
                     | CAN_TESTER_ADDR)
#define ISOTP_TX_ID (0x18DA0000U \
                     | ((uint32_t)CAN_TESTER_ADDR << 8) \
                     | CAN_ECU_ADDR_DEFAULT)
#define ISOTP_FUNC_RX_ID 0x18DB33F1U

static isotp_channel_t s_phys;

static bool isotp_tx_cb(const can_frame_t *f, void *user) {
    (void)user;
    return can_hw_send(f);
}

/* ── ECUReset hook ───────────────────────────────────────────────── */

static void ecu_reset_hook(uint8_t sub_function) {
    /* sub 0x01 = hardReset, 0x03 = softReset; both map to a
     * watchdog reboot. Sleep briefly so the positive response has a
     * chance to leave on the wire before we actually reset. */
    (void)sub_function;
    sleep_ms(50);
    watchdog_reboot(0, 0, 0);
    while (1) tight_loop_contents();  /* unreached */
}

static const uds_app_config_t s_app_cfg = {
    .ecu_reset_hook   = ecu_reset_hook,
};

/* ── DID seeding ─────────────────────────────────────────────────── */
/* 0xF187 = ISO 14229 ASAM ODX-D Spare Part Number
 * 0xF195 = ISO 14229 ASAM ODX-D System Supplier ECU Software Version
 * 0xF18C = ISO 14229 ECU Serial Number */

static void seed_did_store(void) {
    did_store_init();

    static const char *spare_part = "sumo-rp2350-checkpoint-4a";
    did_store_add(0xF187,
                  (const uint8_t *)spare_part, (uint8_t)strlen(spare_part),
                  /*writable=*/false, DID_ACCESS_PUBLIC);

    static const char *sw_version = "0.1.0-uds-bringup";
    did_store_add(0xF195,
                  (const uint8_t *)sw_version, (uint8_t)strlen(sw_version),
                  false, DID_ACCESS_PUBLIC);

    static const char *vendor = "tr-sdv-sandbox";
    did_store_add(0xF18C,
                  (const uint8_t *)vendor, (uint8_t)strlen(vendor),
                  false, DID_ACCESS_PUBLIC);
}

/* ── Main loop ───────────────────────────────────────────────────── */

int main(void) {
    led_init();
    blink_n(2, 60, 60);

    stdio_init_all();
    sleep_ms(2000);  /* let USB-CDC enumerate */

    printf("\n--- sumo-rp2350 uds-server "
           "(checkpoint 4a: UDS bring-up) ---\n");
    fflush(stdout);

    STAGE(1, "stdio + led ok");

    if (!can_hw_init()) {
        printf("can_hw_init FAILED — check XL2515 wiring + pin map\n");
        fflush(stdout);
        while (1) blink_n(1, 30, 970);
    }
    STAGE(2, "XL2515 init ok @ 500 kbps, 8 MHz xtal");

    isotp_init(&s_phys, ISOTP_RX_ID, ISOTP_TX_ID, isotp_tx_cb, NULL);
    STAGE(3, "ISO-TP channel created");

    seed_did_store();
    STAGE(4, "DID store seeded (F187 F195 F18C)");

    uds_server_init(&s_app_cfg);
    STAGE(5, "UDS server initialised");

    printf("Listening on RX=0x%08x  TX=0x%08x  (29-bit)\n",
           (unsigned)ISOTP_RX_ID, (unsigned)ISOTP_TX_ID);
    fflush(stdout);

    uint32_t last_blink_ms = 0;

    for (;;) {
        /* CAN RX → ISO-TP. mcp2515_receive is non-blocking, returns
         * false when the FIFO is empty. */
        can_frame_t rx;
        while (can_hw_receive(&rx)) {
            if (rx.id == ISOTP_RX_ID || rx.id == ISOTP_FUNC_RX_ID) {
                isotp_on_rx(&s_phys, &rx);
            }
        }

        /* ISO-TP timers / CF transmission. */
        isotp_poll(&s_phys);

        /* If a complete UDS request is ready, dispatch it. */
        if (isotp_rx_ready(&s_phys)) {
            uint16_t req_len;
            const uint8_t *req = isotp_rx_data(&s_phys, &req_len);

            printf("UDS req: %02x", req[0]);
            for (uint16_t i = 1; i < req_len && i < 8; i++) printf(" %02x", req[i]);
            if (req_len > 8) printf(" …(%u B)", req_len);
            printf("\n");
            fflush(stdout);

            uds_response_t resp;
            bool send = uds_server_process(req, req_len,
                                           /*functional=*/false, &resp);
            isotp_rx_done(&s_phys);

            if (send) {
                printf("UDS rsp: %02x (%u B)\n", resp.data[0], resp.len);
                fflush(stdout);
                isotp_send(&s_phys, resp.data, resp.len);
            }
        }

        /* Session / security / etc. timers. */
        uds_server_poll();

        /* 1-Hz heartbeat blink so we have a lifesign even without a
         * console (the GPIO LED is the truth-source for "device
         * alive"). */
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if ((now - last_blink_ms) >= 500) {
            led_set((now / 500) & 1);
            last_blink_ms = now;
        }
    }
    return 0;
}

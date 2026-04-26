/*
 * MCP2515 driver — interrupt-driven, single-TXB.
 *
 * The /INT pin (PIN_MCP2515_INT, GP8 on Waveshare RP2350-CAN) is wired
 * to a GPIO falling-edge IRQ. The ISR drains every flag set in CANINTF
 * in a loop:
 *
 *   RX0IF / RX1IF — read the corresponding RX buffer into the
 *                   software RX FIFO; main thread drains via
 *                   mcp2515_receive() in O(1).
 *
 *   TX0IF         — pop the next pending frame from the software TX
 *                   FIFO into TXB0 and kick RTS. If the FIFO is empty,
 *                   mark TXB0 idle so the next mcp2515_send() can kick
 *                   directly.
 *
 * Only TXB0 is ever loaded — the MCP2515 reorders frames among
 * equal-priority TXBs in "highest-buffer-wins" order (datasheet §3.6),
 * which scrambles ISO-TP CF sequence under any back-to-back enqueue.
 * Sticking to TXB0 makes the wire order trivially insertion-order.
 *
 * Priority queues — DEFERRED: today every outbound frame is on
 * 0x18DAF142 (physical UDS response) so a single FIFO is correct. When
 * we add a second outbound class (functional broadcasts, periodic
 * DIDs, error frames during OTA), replace s_tx_buf / s_tx_head /
 * s_tx_tail with a small array of FIFOs and let the ISR pop from the
 * highest non-empty band. The boundary between bands MUST be at
 * ISO-TP-message granularity — never interleave CFs from two messages
 * on the same CAN-ID — which is naturally enforced today because
 * isotp.c only emits CFs sequentially per channel.
 *
 * SPI access is allowed from this ISR. mcp2515_send() saves and
 * disables interrupts around its critical section so the main thread
 * and ISR never share the SPI bus mid-transaction.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mcp2515.h"
#include "mcp2515_regs.h"
#include "pin_config.h"
#include "app_config.h"

#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include <string.h>

/* ── FIFO sizes ─────────────────────────────────────────────────── */
/*
 * 32 entries × ~16 bytes each = ~512 bytes per FIFO. Sized comfortably
 * above the worst-case ISO-TP burst we expect from 4b's SUIT-over-UDS
 * (a TransferData response is at most one ISO-TP message ≈ 1 FF +
 * ~600 CFs for a 4 KB block). Bump if needed via app_config.h.
 */
#ifndef MCP2515_TX_FIFO_DEPTH
#define MCP2515_TX_FIFO_DEPTH 32
#endif
#ifndef MCP2515_RX_FIFO_DEPTH
#define MCP2515_RX_FIFO_DEPTH 32
#endif

/* ── State (volatile because ISR + main thread share) ──────────── */
static can_frame_t       s_tx_buf[MCP2515_TX_FIFO_DEPTH];
static volatile uint16_t s_tx_head;        /* writer: main thread     */
static volatile uint16_t s_tx_tail;        /* writer: ISR             */

static can_frame_t       s_rx_buf[MCP2515_RX_FIFO_DEPTH];
static volatile uint16_t s_rx_head;        /* writer: ISR             */
static volatile uint16_t s_rx_tail;        /* writer: main thread     */

static volatile bool     s_tx_idle = true; /* TXB0 has no pending TX  */

/* Diagnostic counters — exposed via mcp2515_get_drops() for telemetry
 * if/when we add an over-the-bus stat. */
static volatile uint32_t s_rx_drops;
static volatile uint32_t s_tx_drops;

/* ── Low-level SPI helpers ────────────────────────────────────────── */

static inline void cs_select(void)   { gpio_put(PIN_MCP2515_CS, 0); }
static inline void cs_deselect(void) { gpio_put(PIN_MCP2515_CS, 1); }

static void spi_write_byte(uint8_t b) {
    spi_write_blocking(PIN_SPI_PORT, &b, 1);
}

static uint8_t spi_read_byte(void) {
    uint8_t b = 0;
    spi_read_blocking(PIN_SPI_PORT, 0x00, &b, 1);
    return b;
}

/* ── Register access ──────────────────────────────────────────────── */

uint8_t mcp2515_read_reg(uint8_t addr) {
    cs_select();
    spi_write_byte(MCP_READ);
    spi_write_byte(addr);
    uint8_t val = spi_read_byte();
    cs_deselect();
    return val;
}

static void mcp2515_read_regs(uint8_t addr, uint8_t *buf, uint8_t len) {
    cs_select();
    spi_write_byte(MCP_READ);
    spi_write_byte(addr);
    spi_read_blocking(PIN_SPI_PORT, 0x00, buf, len);
    cs_deselect();
}

static void mcp2515_write_reg(uint8_t addr, uint8_t val) {
    cs_select();
    spi_write_byte(MCP_WRITE);
    spi_write_byte(addr);
    spi_write_byte(val);
    cs_deselect();
}

static void mcp2515_write_regs(uint8_t addr, const uint8_t *buf, uint8_t len) {
    cs_select();
    spi_write_byte(MCP_WRITE);
    spi_write_byte(addr);
    spi_write_blocking(PIN_SPI_PORT, buf, len);
    cs_deselect();
}

static void mcp2515_bit_modify(uint8_t addr, uint8_t mask, uint8_t val) {
    cs_select();
    spi_write_byte(MCP_BIT_MODIFY);
    spi_write_byte(addr);
    spi_write_byte(mask);
    spi_write_byte(val);
    cs_deselect();
}

uint8_t mcp2515_read_status(void) {
    cs_select();
    spi_write_byte(MCP_READ_STATUS);
    uint8_t s = spi_read_byte();
    cs_deselect();
    return s;
}

static void mcp2515_reset(void) {
    cs_select();
    spi_write_byte(MCP_RESET);
    cs_deselect();
    sleep_ms(10);
}

/* ── Mode control ─────────────────────────────────────────────────── */

bool mcp2515_set_mode(uint8_t mode) {
    mcp2515_bit_modify(REG_CANCTRL, MODE_MASK, mode);
    for (int i = 0; i < 100; i++) {
        uint8_t stat = mcp2515_read_reg(REG_CANSTAT);
        if ((stat & CANSTAT_OPMOD_MASK) == mode) return true;
        sleep_us(100);
    }
    return false;
}

/* ── Filter configuration ─────────────────────────────────────────── */

static void write_ext_id_regs(uint8_t base_addr, uint32_t id29, bool is_filter) {
    uint16_t sid = (uint16_t)(id29 >> 18);
    uint32_t eid = id29 & 0x3FFFF;

    uint8_t sidh = (uint8_t)(sid >> 3);
    uint8_t sidl = (uint8_t)((sid & 0x07) << 5);
    if (is_filter) sidl |= 0x08;
    sidl |= (uint8_t)((eid >> 16) & 0x03);
    uint8_t eid8 = (uint8_t)(eid >> 8);
    uint8_t eid0 = (uint8_t)(eid & 0xFF);

    mcp2515_write_reg(base_addr,     sidh);
    mcp2515_write_reg(base_addr + 1, sidl);
    mcp2515_write_reg(base_addr + 2, eid8);
    mcp2515_write_reg(base_addr + 3, eid0);
}

static void write_std_id_regs(uint8_t base_addr, uint16_t id11) {
    mcp2515_write_reg(base_addr,     (uint8_t)(id11 >> 3));
    mcp2515_write_reg(base_addr + 1, (uint8_t)((id11 & 0x07) << 5));
}

void mcp2515_set_filter(uint32_t filter0_id, uint32_t filter1_id, bool extended) {
    if (extended) {
        write_ext_id_regs(REG_RXM0SIDH, 0x1FFFFFFF, false);
        write_ext_id_regs(REG_RXM1SIDH, 0x1FFFFFFF, false);
        write_ext_id_regs(REG_RXF0SIDH, filter0_id, true);
        write_ext_id_regs(REG_RXF1SIDH, filter0_id, true);
        write_ext_id_regs(REG_RXF2SIDH, filter1_id, true);
        write_ext_id_regs(REG_RXF3SIDH, filter1_id, true);
        write_ext_id_regs(REG_RXF4SIDH, filter1_id, true);
        write_ext_id_regs(REG_RXF5SIDH, filter1_id, true);
    } else {
        mcp2515_write_reg(REG_RXM0SIDH, 0xFF);
        mcp2515_write_reg(REG_RXM0SIDL, 0xE0);
        mcp2515_write_reg(REG_RXM1SIDH, 0xFF);
        mcp2515_write_reg(REG_RXM1SIDL, 0xE0);
        write_std_id_regs(REG_RXF0SIDH, (uint16_t)filter0_id);
        write_std_id_regs(REG_RXF1SIDH, (uint16_t)filter0_id);
        write_std_id_regs(REG_RXF2SIDH, (uint16_t)filter1_id);
        write_std_id_regs(REG_RXF3SIDH, (uint16_t)filter1_id);
        write_std_id_regs(REG_RXF4SIDH, (uint16_t)filter1_id);
        write_std_id_regs(REG_RXF5SIDH, (uint16_t)filter1_id);
    }

    mcp2515_write_reg(REG_RXB0CTRL, RXB_RXM_FILTER | RXB_BUKT);
    mcp2515_write_reg(REG_RXB1CTRL, RXB_RXM_FILTER);
}

bool mcp2515_update_filter(uint32_t filter0_id, uint32_t filter1_id, bool extended) {
    if (!mcp2515_set_mode(MODE_CONFIG)) return false;
    mcp2515_set_filter(filter0_id, filter1_id, extended);
    return mcp2515_set_mode(MODE_NORMAL);
}

/* ── TX low-level: load frame into TXB0 and kick RTS ──────────── */

static void load_and_kick_txb0(const can_frame_t *frame) {
    if (frame->is_ext) {
        uint16_t sid = (uint16_t)(frame->id >> 18);
        uint32_t eid = frame->id & 0x3FFFF;

        uint8_t sidh = (uint8_t)(sid >> 3);
        uint8_t sidl = (uint8_t)((sid & 0x07) << 5) | 0x08
                       | (uint8_t)((eid >> 16) & 0x03);
        uint8_t eid8 = (uint8_t)(eid >> 8);
        uint8_t eid0 = (uint8_t)(eid & 0xFF);

        mcp2515_write_reg(REG_TXB0SIDH,     sidh);
        mcp2515_write_reg(REG_TXB0SIDH + 1, sidl);
        mcp2515_write_reg(REG_TXB0SIDH + 2, eid8);
        mcp2515_write_reg(REG_TXB0SIDH + 3, eid0);
    } else {
        uint8_t sidh = (uint8_t)(frame->id >> 3);
        uint8_t sidl = (uint8_t)((frame->id & 0x07) << 5);
        mcp2515_write_reg(REG_TXB0SIDH,     sidh);
        mcp2515_write_reg(REG_TXB0SIDH + 1, sidl);
    }

    uint8_t dlc = frame->dlc & 0x0F;
    if (frame->is_rtr) dlc |= 0x40;
    mcp2515_write_reg(REG_TXB0DLC, dlc);

    if (frame->dlc > 0 && !frame->is_rtr) {
        mcp2515_write_regs(REG_TXB0D0, frame->data, frame->dlc);
    }

    cs_select();
    spi_write_byte(MCP_RTS(0));
    cs_deselect();
}

/* ── RX low-level: read RXBn into a target slot ───────────────── */

static void read_rxb(uint8_t rxb_sidh_reg, uint8_t rxb_dlc_reg,
                     uint8_t rxb_d0_reg, can_frame_t *out) {
    uint8_t id_buf[4];
    mcp2515_read_regs(rxb_sidh_reg, id_buf, 4);

    uint8_t sidh = id_buf[0];
    uint8_t sidl = id_buf[1];

    out->is_ext = (sidl & 0x08) != 0;
    out->id = ((uint32_t)sidh << 3) | (sidl >> 5);

    if (out->is_ext) {
        out->id = (out->id << 18) |
                  ((uint32_t)(sidl & 0x03) << 16) |
                  ((uint32_t)id_buf[2] << 8) |
                  id_buf[3];
    }

    uint8_t raw_dlc = mcp2515_read_reg(rxb_dlc_reg);
    out->dlc = raw_dlc & 0x0F;
    out->is_rtr = (raw_dlc & 0x40) != 0;
    if (out->dlc > 8) out->dlc = 8;

    if (out->dlc > 0) {
        mcp2515_read_regs(rxb_d0_reg, out->data, out->dlc);
    }
}

static void rx_fifo_push(const can_frame_t *frame) {
    uint16_t next_head = (s_rx_head + 1) % MCP2515_RX_FIFO_DEPTH;
    if (next_head == s_rx_tail) {
        s_rx_drops++;
        return;
    }
    s_rx_buf[s_rx_head] = *frame;
    __dmb();
    s_rx_head = next_head;
}

/* ── ISR: drain CANINTF until quiet ────────────────────────────── */

static void mcp2515_isr(uint gpio, uint32_t events) {
    (void)events;
    if (gpio != PIN_MCP2515_INT) return;

    /* /INT is held low while ANY enabled flag in CANINTF is set. We
     * use edge-fall (won't refire if line stays low), so we have to
     * drain every set flag in one ISR invocation. Loop until no
     * serviceable flag remains. */
    for (;;) {
        uint8_t f = mcp2515_read_reg(REG_CANINTF);
        if ((f & (CANINTF_RX0IF | CANINTF_RX1IF | CANINTF_TX0IF)) == 0) {
            break;
        }

        if (f & CANINTF_RX0IF) {
            can_frame_t tmp;
            read_rxb(REG_RXB0SIDH, REG_RXB0DLC, REG_RXB0D0, &tmp);
            mcp2515_bit_modify(REG_CANINTF, CANINTF_RX0IF, 0);
            rx_fifo_push(&tmp);
        }
        if (f & CANINTF_RX1IF) {
            can_frame_t tmp;
            read_rxb(REG_RXB1SIDH, REG_RXB1DLC, REG_RXB1D0, &tmp);
            mcp2515_bit_modify(REG_CANINTF, CANINTF_RX1IF, 0);
            rx_fifo_push(&tmp);
        }
        if (f & CANINTF_TX0IF) {
            mcp2515_bit_modify(REG_CANINTF, CANINTF_TX0IF, 0);

            if (s_tx_head != s_tx_tail) {
                /* Pop next from ring, copy out (so we can advance
                 * tail before the slow SPI sequence), kick TXB0. */
                can_frame_t next_frame = s_tx_buf[s_tx_tail];
                __dmb();
                s_tx_tail = (s_tx_tail + 1) % MCP2515_TX_FIFO_DEPTH;
                load_and_kick_txb0(&next_frame);
            } else {
                s_tx_idle = true;
            }
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

bool mcp2515_send(const can_frame_t *frame) {
    /* save_and_disable_interrupts blanks the BASEPRI/PRIMASK; this
     * makes the (idle-check → fast-path-kick / slow-path-enqueue)
     * pair atomic with respect to mcp2515_isr. SPI work inside is
     * ~5 µs at 8 MHz over 12-byte frame load — negligible. */
    uint32_t save = save_and_disable_interrupts();

    /* Fast path: TXB0 idle and ring empty → kick directly, skip ring. */
    if (s_tx_idle) {
        s_tx_idle = false;
        load_and_kick_txb0(frame);
        restore_interrupts(save);
        return true;
    }

    /* Slow path: TXB0 busy → push to ring, ISR will pop on next TX0IF. */
    uint16_t next_head = (s_tx_head + 1) % MCP2515_TX_FIFO_DEPTH;
    if (next_head == s_tx_tail) {
        s_tx_drops++;
        restore_interrupts(save);
        return false;  /* ring full */
    }
    s_tx_buf[s_tx_head] = *frame;
    __dmb();
    s_tx_head = next_head;

    restore_interrupts(save);
    return true;
}

bool mcp2515_receive(can_frame_t *frame) {
    if (s_rx_head == s_rx_tail) return false;
    *frame = s_rx_buf[s_rx_tail];
    __dmb();
    s_rx_tail = (s_rx_tail + 1) % MCP2515_RX_FIFO_DEPTH;
    return true;
}

void mcp2515_get_errors(uint8_t *tec, uint8_t *rec) {
    if (tec) *tec = mcp2515_read_reg(REG_TEC);
    if (rec) *rec = mcp2515_read_reg(REG_REC);
}

void mcp2515_get_drops(uint32_t *tx_drops, uint32_t *rx_drops) {
    if (tx_drops) *tx_drops = s_tx_drops;
    if (rx_drops) *rx_drops = s_rx_drops;
}

/* ── Initialisation ──────────────────────────────────────────────── */

bool mcp2515_init(void) {
    /* SPI + GPIO pin setup */
    spi_init(PIN_SPI_PORT, MCP2515_SPI_FREQ_HZ);
    gpio_set_function(PIN_SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MISO, GPIO_FUNC_SPI);

    gpio_init(PIN_MCP2515_CS);
    gpio_set_dir(PIN_MCP2515_CS, GPIO_OUT);
    cs_deselect();

    gpio_init(PIN_MCP2515_INT);
    gpio_set_dir(PIN_MCP2515_INT, GPIO_IN);
    gpio_pull_up(PIN_MCP2515_INT);

    /* Software reset chip */
    mcp2515_reset();

    uint8_t stat = mcp2515_read_reg(REG_CANSTAT);
    if ((stat & CANSTAT_OPMOD_MASK) != MODE_CONFIG) return false;

    /* Bit timing: 500 kbps with 8 MHz xtal */
    mcp2515_write_reg(REG_CNF1, MCP_CNF1_500K);
    mcp2515_write_reg(REG_CNF2, MCP_CNF2_500K);
    mcp2515_write_reg(REG_CNF3, MCP_CNF3_500K);

    /* Acceptance filters: physical + functional UDS addressing. */
    {
        uint32_t phys_rx = 0x18DA0000UL
                         | ((uint32_t)CAN_ECU_ADDR_DEFAULT << 8)
                         | CAN_TESTER_ADDR;
        uint32_t func_rx = 0x18DB33F1UL;
        mcp2515_set_filter(phys_rx, func_rx, CAN_USE_EXT_ID);
    }

    /* Reset software state — must precede enabling the GPIO IRQ,
     * which could otherwise see stale ring indices on a warm-restart. */
    s_tx_head = s_tx_tail = 0;
    s_rx_head = s_rx_tail = 0;
    s_tx_idle = true;
    s_rx_drops = s_tx_drops = 0;

    /* Enable RX0/RX1/TX0 interrupts at the chip; clear all flags. */
    mcp2515_write_reg(REG_CANINTE,
                      CANINTE_RX0IE | CANINTE_RX1IE | CANINTE_TX0IE);
    mcp2515_write_reg(REG_CANINTF, 0x00);

    /* Wire /INT pin → mcp2515_isr on falling edge BEFORE switching
     * to NORMAL mode, so we don't miss an RX that arrives the moment
     * the chip starts listening. /INT is high right now (no flags
     * pending), so no spurious IRQ fires here. */
    gpio_set_irq_enabled_with_callback(PIN_MCP2515_INT,
                                       GPIO_IRQ_EDGE_FALL,
                                       true, mcp2515_isr);

    /* Switch to NORMAL mode — chip starts arbitrating on the bus. */
    return mcp2515_set_mode(MODE_NORMAL);
}

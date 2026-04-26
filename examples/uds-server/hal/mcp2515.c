#include "mcp2515.h"
#include "mcp2515_regs.h"
#include "pin_config.h"
#include "app_config.h"

#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

#include <string.h>

/* ── Low-level SPI helpers ────────────────────────────────────────── */

static inline void cs_select(void) {
    gpio_put(PIN_MCP2515_CS, 0);
}

static inline void cs_deselect(void) {
    gpio_put(PIN_MCP2515_CS, 1);
}

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
    /* Wait for mode to take effect (max ~10ms) */
    for (int i = 0; i < 100; i++) {
        uint8_t stat = mcp2515_read_reg(REG_CANSTAT);
        if ((stat & CANSTAT_OPMOD_MASK) == mode) {
            return true;
        }
        sleep_us(100);
    }
    return false;
}

/* ── Filter configuration ─────────────────────────────────────────── */

/*
 * Write a 29-bit extended ID into MCP2515 filter/mask register set.
 * base_addr = SIDH register address (SIDL, EID8, EID0 follow at +1,+2,+3).
 * For filters: set EXIDE bit in SIDL to require extended frame match.
 * For masks:   EXIDE bit is not used (set to 0).
 */
static void write_ext_id_regs(uint8_t base_addr, uint32_t id29, bool is_filter) {
    /* 29-bit ID split:
     *   SID[10:0] = id29[28:18]
     *   EID[17:0] = id29[17:0]
     *
     * Register layout:
     *   SIDH = SID[10:3]
     *   SIDL = SID[2:0]<<5 | EXIDE<<3 | EID[17:16]
     *   EID8 = EID[15:8]
     *   EID0 = EID[7:0]
     */
    uint16_t sid = (uint16_t)(id29 >> 18);
    uint32_t eid = id29 & 0x3FFFF;

    uint8_t sidh = (uint8_t)(sid >> 3);
    uint8_t sidl = (uint8_t)((sid & 0x07) << 5);
    if (is_filter) sidl |= 0x08;           /* EXIDE bit = match extended only */
    sidl |= (uint8_t)((eid >> 16) & 0x03); /* EID[17:16] */
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
    /*
     * RXB0: mask0 + filter0,filter1 -> accept filter0_id (physical)
     * RXB1: mask1 + filter2..5     -> accept filter1_id (functional)
     */
    if (extended) {
        /* Mask: all 29 bits must match */
        write_ext_id_regs(REG_RXM0SIDH, 0x1FFFFFFF, false);
        write_ext_id_regs(REG_RXM1SIDH, 0x1FFFFFFF, false);

        /* Filters 0,1 -> RXB0: physical */
        write_ext_id_regs(REG_RXF0SIDH, filter0_id, true);
        write_ext_id_regs(REG_RXF1SIDH, filter0_id, true);

        /* Filters 2..5 -> RXB1: functional */
        write_ext_id_regs(REG_RXF2SIDH, filter1_id, true);
        write_ext_id_regs(REG_RXF3SIDH, filter1_id, true);
        write_ext_id_regs(REG_RXF4SIDH, filter1_id, true);
        write_ext_id_regs(REG_RXF5SIDH, filter1_id, true);
    } else {
        /* Standard 11-bit: mask all bits */
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

    /* RXB0CTRL: use filters, rollover to RXB1 if full */
    mcp2515_write_reg(REG_RXB0CTRL, RXB_RXM_FILTER | RXB_BUKT);
    /* RXB1CTRL: use filters */
    mcp2515_write_reg(REG_RXB1CTRL, RXB_RXM_FILTER);
}

bool mcp2515_update_filter(uint32_t filter0_id, uint32_t filter1_id, bool extended) {
    if (!mcp2515_set_mode(MODE_CONFIG)) return false;
    mcp2515_set_filter(filter0_id, filter1_id, extended);
    return mcp2515_set_mode(MODE_NORMAL);
}

/* ── Initialisation ───────────────────────────────────────────────── */

bool mcp2515_init(void) {
    /* Init SPI pins */
    spi_init(PIN_SPI_PORT, MCP2515_SPI_FREQ_HZ);
    gpio_set_function(PIN_SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MISO, GPIO_FUNC_SPI);

    /* CS pin as GPIO output, deselected */
    gpio_init(PIN_MCP2515_CS);
    gpio_set_dir(PIN_MCP2515_CS, GPIO_OUT);
    cs_deselect();

    /* INT pin as input (active low from MCP2515) */
    gpio_init(PIN_MCP2515_INT);
    gpio_set_dir(PIN_MCP2515_INT, GPIO_IN);
    gpio_pull_up(PIN_MCP2515_INT);

    /* Software reset */
    mcp2515_reset();

    /* Verify we are in config mode after reset */
    uint8_t stat = mcp2515_read_reg(REG_CANSTAT);
    if ((stat & CANSTAT_OPMOD_MASK) != MODE_CONFIG) {
        return false;
    }

    /* Bit timing: 500kbps with 8 MHz oscillator */
    mcp2515_write_reg(REG_CNF1, MCP_CNF1_500K);
    mcp2515_write_reg(REG_CNF2, MCP_CNF2_500K);
    mcp2515_write_reg(REG_CNF3, MCP_CNF3_500K);

    /* Set up acceptance filters using default ECU address.
     * Caller can update filters at runtime via mcp2515_update_filter(). */
    {
        uint32_t phys_rx = 0x18DA0000UL
                         | ((uint32_t)CAN_ECU_ADDR_DEFAULT << 8)
                         | CAN_TESTER_ADDR;
        uint32_t func_rx = 0x18DB33F1UL;
        mcp2515_set_filter(phys_rx, func_rx, CAN_USE_EXT_ID);
    }

    /* Enable RX interrupts only */
    mcp2515_write_reg(REG_CANINTE, CANINTE_RX0IE | CANINTE_RX1IE);

    /* Clear all interrupt flags */
    mcp2515_write_reg(REG_CANINTF, 0x00);

    /* Switch to normal mode */
    if (!mcp2515_set_mode(MODE_NORMAL)) {
        return false;
    }

    return true;
}

/* ── Transmit ─────────────────────────────────────────────────────── */
/*
 * Single-TXB-busy-wait strategy (TXB0 only).
 *
 * The MCP2515 has three TX buffers and, when more than one is loaded
 * simultaneously, transmits the highest-numbered first among equal-
 * priority buffers (datasheet §3.6). With our single-core polled main
 * loop, ISO-TP CFs are pushed back-to-back fast enough that all three
 * TXBs end up loaded before TXB0 has finished, and the second/third
 * CFs land on the wire out of order — the receiver then NAKs on the
 * sequence-number check. (The RP2040 reference dodges this by virtue
 * of its dual-core split, which paces TX naturally.)
 *
 * Quick fix: always use TXB0, busy-wait for TXREQ to clear before
 * writing the next frame. Cost is ~250 µs blocking per frame at
 * 500 kbps — invisible at our throughput. The proper fix is
 * interrupt-driven TX with a software FIFO; tracked in
 * docs/tx-irq-plan.md.
 */

bool mcp2515_send(const can_frame_t *frame) {
    static const uint8_t txb_ctrl[] = { REG_TXB0CTRL };
    static const uint8_t txb_sidh[] = { REG_TXB0SIDH };
    static const uint8_t txb_d0[]   = { REG_TXB0D0   };
    static const uint8_t txb_dlc[]  = { REG_TXB0DLC  };

    /* Busy-wait until TXB0 is idle. Bounded by ~250 µs (one CAN frame
     * worth at 500 kbps) under healthy bus conditions; ~5 ms cap to
     * surface a stuck bus rather than block forever. */
    const int TIMEOUT_US = 5000;
    int waited = 0;
    while (mcp2515_read_reg(txb_ctrl[0]) & TXB_TXREQ) {
        if (waited >= TIMEOUT_US) return false;
        sleep_us(50);
        waited += 50;
    }
    int buf = 0;

    /* Load ID into SIDH/SIDL/EID8/EID0 */
    if (frame->is_ext) {
        /* 29-bit extended: SID = id[28:18], EID = id[17:0] */
        uint16_t sid = (uint16_t)(frame->id >> 18);
        uint32_t eid = frame->id & 0x3FFFF;

        uint8_t sidh = (uint8_t)(sid >> 3);
        uint8_t sidl = (uint8_t)((sid & 0x07) << 5) | 0x08 /* EXIDE */
                       | (uint8_t)((eid >> 16) & 0x03);
        uint8_t eid8 = (uint8_t)(eid >> 8);
        uint8_t eid0 = (uint8_t)(eid & 0xFF);

        mcp2515_write_reg(txb_sidh[buf],     sidh);
        mcp2515_write_reg(txb_sidh[buf] + 1, sidl);
        mcp2515_write_reg(txb_sidh[buf] + 2, eid8);
        mcp2515_write_reg(txb_sidh[buf] + 3, eid0);
    } else {
        /* 11-bit standard */
        uint8_t sidh = (uint8_t)(frame->id >> 3);
        uint8_t sidl = (uint8_t)((frame->id & 0x07) << 5);

        mcp2515_write_reg(txb_sidh[buf],     sidh);
        mcp2515_write_reg(txb_sidh[buf] + 1, sidl);
    }

    /* DLC */
    uint8_t dlc = frame->dlc & 0x0F;
    if (frame->is_rtr) {
        dlc |= 0x40;
    }
    mcp2515_write_reg(txb_dlc[buf], dlc);

    /* Data bytes */
    if (frame->dlc > 0 && !frame->is_rtr) {
        mcp2515_write_regs(txb_d0[buf], frame->data, frame->dlc);
    }

    /* Request to send */
    cs_select();
    spi_write_byte(MCP_RTS(buf));
    cs_deselect();

    return true;
}

/* ── Receive ──────────────────────────────────────────────────────── */

bool mcp2515_receive(can_frame_t *frame) {
    uint8_t intf = mcp2515_read_reg(REG_CANINTF);

    uint8_t rxb_sidh, rxb_dlc, rxb_d0, rxb_flag;

    if (intf & CANINTF_RX0IF) {
        rxb_sidh = REG_RXB0SIDH;
        rxb_dlc  = REG_RXB0DLC;
        rxb_d0   = REG_RXB0D0;
        rxb_flag = CANINTF_RX0IF;
    } else if (intf & CANINTF_RX1IF) {
        rxb_sidh = REG_RXB1SIDH;
        rxb_dlc  = REG_RXB1DLC;
        rxb_d0   = REG_RXB1D0;
        rxb_flag = CANINTF_RX1IF;
    } else {
        return false; /* No message available */
    }

    /* Read ID registers */
    uint8_t id_buf[4];
    mcp2515_read_regs(rxb_sidh, id_buf, 4);

    uint8_t sidh = id_buf[0];
    uint8_t sidl = id_buf[1];

    frame->is_ext = (sidl & 0x08) != 0;
    frame->id = ((uint32_t)sidh << 3) | (sidl >> 5);

    if (frame->is_ext) {
        /* Extended ID: add EID17:EID0 */
        frame->id = (frame->id << 18) |
                     ((uint32_t)(sidl & 0x03) << 16) |
                     ((uint32_t)id_buf[2] << 8) |
                     id_buf[3];
    }

    /* Read DLC */
    uint8_t raw_dlc = mcp2515_read_reg(rxb_dlc);
    frame->dlc = raw_dlc & 0x0F;
    frame->is_rtr = (raw_dlc & 0x40) != 0;

    if (frame->dlc > 8) frame->dlc = 8;

    /* Read data */
    if (frame->dlc > 0) {
        mcp2515_read_regs(rxb_d0, frame->data, frame->dlc);
    }

    /* Clear interrupt flag */
    mcp2515_bit_modify(REG_CANINTF, rxb_flag, 0x00);

    return true;
}

/* ── Error counters ───────────────────────────────────────────────── */

void mcp2515_get_errors(uint8_t *tec, uint8_t *rec) {
    if (tec) *tec = mcp2515_read_reg(REG_TEC);
    if (rec) *rec = mcp2515_read_reg(REG_REC);
}

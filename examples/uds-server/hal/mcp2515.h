#ifndef MCP2515_H
#define MCP2515_H

#include "isotp/can_frame.h"

/* ── Public API ───────────────────────────────────────────────────── */

/**
 * Initialise MCP2515: reset, configure bit timing for 500kbps,
 * set up acceptance filters, switch to normal mode.
 * Returns true on success.
 */
bool mcp2515_init(void);

/**
 * Enqueue a CAN frame for transmission. Non-blocking. Frame is loaded
 * into TXB0 immediately if the chip is idle, otherwise queued in a
 * software FIFO; the /INT-driven ISR pops the FIFO each time TXB0
 * finishes transmitting. Returns true if the frame was accepted; false
 * if the FIFO is full. Single TXB used → wire order = enqueue order.
 */
bool mcp2515_send(const can_frame_t *frame);

/**
 * Pop a received frame from the software FIFO. Non-blocking; the ISR
 * fills the FIFO from RXB0/RXB1 as frames arrive. Returns true if a
 * frame was copied into *frame, false if the FIFO is empty.
 */
bool mcp2515_receive(can_frame_t *frame);

/**
 * Set acceptance filters for physical and functional CAN IDs.
 * Supports both 11-bit standard and 29-bit extended IDs.
 * filter0 -> RXB0 (physical), filter1 -> RXB1 (functional).
 * Must be called while in config mode (called internally by mcp2515_init).
 */
void mcp2515_set_filter(uint32_t filter0_id, uint32_t filter1_id, bool extended);

/**
 * Update acceptance filters at runtime.
 * Enters config mode, sets new filters, returns to normal mode.
 * Returns true on success.
 */
bool mcp2515_update_filter(uint32_t filter0_id, uint32_t filter1_id, bool extended);

/**
 * Read a single register.
 */
uint8_t mcp2515_read_reg(uint8_t addr);

/**
 * Read the status byte (READ_STATUS instruction).
 */
uint8_t mcp2515_read_status(void);

/**
 * Get transmit/receive error counters.
 */
void mcp2515_get_errors(uint8_t *tec, uint8_t *rec);

/**
 * Get cumulative TX / RX FIFO drop counts. Drops happen when
 * mcp2515_send / the ISR can't push a new frame because the FIFO is
 * full. Useful for diagnostics; non-monotonic across init.
 */
void mcp2515_get_drops(uint32_t *tx_drops, uint32_t *rx_drops);

#endif /* MCP2515_H */

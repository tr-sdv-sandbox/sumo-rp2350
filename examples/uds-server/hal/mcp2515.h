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
 * Send a CAN frame.  Tries TXB0 first, then TXB1, TXB2.
 * Returns true if the frame was loaded into a transmit buffer.
 */
bool mcp2515_send(const can_frame_t *frame);

/**
 * Check if a received frame is available (non-blocking).
 * If available, copies it into *frame and returns true.
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

#endif /* MCP2515_H */

/*
 * RP2350 binding of uds-tiny's recommended CAN HAL.
 *
 * Forwards to the verbatim-from-RP2040 mcp2515 driver, which already
 * exposes mcp2515_init/send/receive/update_filter with matching
 * signatures. Five lines because the can_hw.h boundary was designed
 * for exactly this case.
 */
#include "uds_tiny/can_hw.h"
#include "mcp2515.h"

bool can_hw_init(void) { return mcp2515_init(); }
bool can_hw_send(const can_frame_t *frame) { return mcp2515_send(frame); }
bool can_hw_receive(can_frame_t *frame) { return mcp2515_receive(frame); }
bool can_hw_update_filter(uint32_t f0, uint32_t f1, bool ext) {
    return mcp2515_update_filter(f0, f1, ext);
}

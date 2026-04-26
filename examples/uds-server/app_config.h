#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/*
 * Per-application defines for the MCP2515 driver and CAN-ID layout.
 *
 * NOTE: this header deliberately does NOT override uds-tiny's library
 * sizing knobs (ISOTP_RX_BUF_SIZE, DID_MAX_ENTRIES, ...). The lib
 * compiles into separate static libraries with their own sizes baked
 * into struct layouts; if the app overrides those, the lib and app
 * disagree on sizeof(isotp_channel_t) etc. and the app's static
 * allocations get smashed by lib-side memset()s. Stick to lib defaults
 * here; bump them at the lib level (in uds-tiny) when needed.
 */

#include <stdint.h>

/* ── CAN identifiers (mcp2515.c reads these directly) ───────────── */
#define CAN_ECU_ADDR_DEFAULT 0x42
#define CAN_TESTER_ADDR      0xF1
#define CAN_USE_EXT_ID       1
#define CAN_BAUDRATE_KBPS    500

/* ── MCP2515 SW FIFO depths ──────────────────────────────────────
 * Default 32 isn't enough for the CF burst of a 1 KB UDS
 * TransferData message (1 FF + 146 CFs). 256 holds ~4 KB worth of
 * frames — plenty for a full ISO-TP message at the host's max
 * block size of 1024. */
#define MCP2515_RX_FIFO_DEPTH 256
#define MCP2515_TX_FIFO_DEPTH 64

#endif /* APP_CONFIG_H */

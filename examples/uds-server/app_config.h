#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/*
 * Application-level overrides for uds-tiny's library defaults.
 *
 * Compile this header in via -DUDS_APP_CONFIG="\"app_config.h\"",
 * -DSTORE_APP_CONFIG=..., -DISOTP_APP_CONFIG=...   (see CMakeLists.txt).
 *
 * Every #define here either overrides a library default or supplies a
 * value the MCP2515 driver references directly (CAN addressing,
 * extended-ID flag, etc — those are NOT library knobs).
 */

#include <stdint.h>

/* ── CAN identifiers (mcp2515.c reads these directly) ───────────── */
#define CAN_ECU_ADDR_DEFAULT 0x42
#define CAN_TESTER_ADDR      0xF1
#define CAN_USE_EXT_ID       1
#define CAN_BAUDRATE_KBPS    500

/* ── ISO-TP buffers — small for MVP, no SUIT envelopes yet ──────── */
#define ISOTP_RX_BUF_SIZE    256
#define ISOTP_TX_BUF_SIZE    256
#define ISOTP_MAX_PAYLOAD    255

/* ── UDS — keep tables small; only 4 services exercised in MVP ──── */
#define UDS_MAX_SERVICES     16
#define DID_MAX_ENTRIES      16
#define DID_MAX_DATA_LEN     32
#define DTC_MAX_ENTRIES      4
#define IO_OUTPUT_MAX        2

#endif /* APP_CONFIG_H */

#ifndef MCP2515_REGS_H
#define MCP2515_REGS_H

/* ── SPI instructions ─────────────────────────────────────────────── */
#define MCP_RESET       0xC0
#define MCP_READ        0x03
#define MCP_WRITE       0x02
#define MCP_READ_STATUS 0xA0
#define MCP_RX_STATUS   0xB0
#define MCP_BIT_MODIFY  0x05

/* Read RX buffer shortcuts (m = 0 or 1) */
#define MCP_READ_RX(m)  (0x90 | ((m) << 2))
/* Load TX buffer shortcuts (n = 0, 1, 2 for TXB0..2; d=0 => ID, d=1 => data) */
#define MCP_LOAD_TX(n, d)  (0x40 | ((n) << 1) | (d))
/* Request to send */
#define MCP_RTS(n)      (0x80 | (1 << (n)))

/* ── Configuration registers ──────────────────────────────────────── */
#define REG_CANSTAT     0x0E
#define REG_CANCTRL     0x0F
#define REG_CNF3        0x28
#define REG_CNF2        0x29
#define REG_CNF1        0x2A

/* ── Interrupt registers ──────────────────────────────────────────── */
#define REG_CANINTE     0x2B
#define REG_CANINTF     0x2C

/* ── Error registers ──────────────────────────────────────────────── */
#define REG_EFLG        0x2D
#define REG_TEC         0x1C
#define REG_REC         0x1D

/* ── TX buffer 0 ──────────────────────────────────────────────────── */
#define REG_TXB0CTRL    0x30
#define REG_TXB0SIDH    0x31
#define REG_TXB0SIDL    0x32
#define REG_TXB0EID8    0x33
#define REG_TXB0EID0    0x34
#define REG_TXB0DLC     0x35
#define REG_TXB0D0      0x36

/* ── TX buffer 1 ──────────────────────────────────────────────────── */
#define REG_TXB1CTRL    0x40
#define REG_TXB1SIDH    0x41
#define REG_TXB1SIDL    0x42
#define REG_TXB1DLC     0x45
#define REG_TXB1D0      0x46

/* ── TX buffer 2 ──────────────────────────────────────────────────── */
#define REG_TXB2CTRL    0x50
#define REG_TXB2SIDH    0x51
#define REG_TXB2SIDL    0x52
#define REG_TXB2DLC     0x55
#define REG_TXB2D0      0x56

/* ── RX buffer 0 ──────────────────────────────────────────────────── */
#define REG_RXB0CTRL    0x60
#define REG_RXB0SIDH    0x61
#define REG_RXB0SIDL    0x62
#define REG_RXB0EID8    0x63
#define REG_RXB0EID0    0x64
#define REG_RXB0DLC     0x65
#define REG_RXB0D0      0x66

/* ── RX buffer 1 ──────────────────────────────────────────────────── */
#define REG_RXB1CTRL    0x70
#define REG_RXB1SIDH    0x71
#define REG_RXB1SIDL    0x72
#define REG_RXB1EID8    0x73
#define REG_RXB1EID0    0x74
#define REG_RXB1DLC     0x75
#define REG_RXB1D0      0x76

/* ── Acceptance filters / masks ───────────────────────────────────── */
#define REG_RXF0SIDH    0x00
#define REG_RXF0SIDL    0x01
#define REG_RXF0EID8    0x02
#define REG_RXF0EID0    0x03
#define REG_RXF1SIDH    0x04
#define REG_RXF1SIDL    0x05
#define REG_RXF1EID8    0x06
#define REG_RXF1EID0    0x07
#define REG_RXF2SIDH    0x08
#define REG_RXF2SIDL    0x09
#define REG_RXF2EID8    0x0A
#define REG_RXF2EID0    0x0B
#define REG_RXF3SIDH    0x10
#define REG_RXF3SIDL    0x11
#define REG_RXF3EID8    0x12
#define REG_RXF3EID0    0x13
#define REG_RXF4SIDH    0x14
#define REG_RXF4SIDL    0x15
#define REG_RXF4EID8    0x16
#define REG_RXF4EID0    0x17
#define REG_RXF5SIDH    0x18
#define REG_RXF5SIDL    0x19
#define REG_RXF5EID8    0x1A
#define REG_RXF5EID0    0x1B

#define REG_RXM0SIDH    0x20
#define REG_RXM0SIDL    0x21
#define REG_RXM0EID8    0x22
#define REG_RXM0EID0    0x23
#define REG_RXM1SIDH    0x24
#define REG_RXM1SIDL    0x25
#define REG_RXM1EID8    0x26
#define REG_RXM1EID0    0x27

/* ── CANCTRL mode bits ────────────────────────────────────────────── */
#define MODE_NORMAL     0x00
#define MODE_SLEEP      0x20
#define MODE_LOOPBACK   0x40
#define MODE_LISTENONLY 0x60
#define MODE_CONFIG     0x80
#define MODE_MASK       0xE0

/* ── CANSTAT bits ─────────────────────────────────────────────────── */
#define CANSTAT_OPMOD_MASK  0xE0

/* ── CANINTF bits ─────────────────────────────────────────────────── */
#define CANINTF_RX0IF   0x01
#define CANINTF_RX1IF   0x02
#define CANINTF_TX0IF   0x04
#define CANINTF_TX1IF   0x08
#define CANINTF_TX2IF   0x10
#define CANINTF_ERRIF   0x20
#define CANINTF_WAKIF   0x40
#define CANINTF_MERRF   0x80

/* ── CANINTE bits ─────────────────────────────────────────────────── */
#define CANINTE_RX0IE   0x01
#define CANINTE_RX1IE   0x02
#define CANINTE_TX0IE   0x04
#define CANINTE_TX1IE   0x08
#define CANINTE_TX2IE   0x10
#define CANINTE_ERRIE   0x20

/* ── TXBnCTRL bits ────────────────────────────────────────────────── */
#define TXB_TXREQ       0x08
#define TXB_ABTF        0x40
#define TXB_MLOA        0x20
#define TXB_TXERR       0x10

/* ── RXBnCTRL bits ────────────────────────────────────────────────── */
#define RXB_RXM_MASK    0x60
#define RXB_RXM_FILTER  0x00    /* Use filters */
#define RXB_RXM_ANY     0x60    /* Accept all messages */
#define RXB_BUKT        0x04    /* Rollover to RXB1 */

/* ── Bit timing for 500kbps with 16 MHz crystal (MCP2515 breakout) ──
 *  TQ = 2*(BRP+1)/Fosc = 2*1/16MHz = 125ns  (BRP=0, prescaler=1)
 *  16 TQ per bit => 500kbps
 *  SyncSeg=1TQ, PropSeg=7TQ, PS1=4TQ, PS2=4TQ  (SJW=1TQ)
 *  Sample point = (1+7+4)/16 = 75%
 *
 *  CNF1: SJW=0(1TQ), BRP=0(prescaler 1)         => 0x00
 *  CNF2: BTLMODE=1, SAM=0, PHSEG1=3(4TQ), PRSEG=6(7TQ) => 0x9E
 *  CNF3: PHSEG2=3(4TQ)                           => 0x03
 */
#define MCP_CNF1_500K   0x00
#define MCP_CNF2_500K   0x9E
#define MCP_CNF3_500K   0x03

/* ── READ_STATUS bit positions ────────────────────────────────────── */
#define STATUS_RX0IF    0x01
#define STATUS_RX1IF    0x02
#define STATUS_TX0REQ   0x04
#define STATUS_TX0IF    0x08
#define STATUS_TX1REQ   0x10
#define STATUS_TX1IF    0x20
#define STATUS_TX2REQ   0x40
#define STATUS_TX2IF    0x80

#endif /* MCP2515_REGS_H */

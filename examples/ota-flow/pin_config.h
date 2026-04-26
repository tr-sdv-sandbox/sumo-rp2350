#ifndef PIN_CONFIG_H
#define PIN_CONFIG_H

/*
 * Waveshare RP2350-CAN — XL2515 (MCP2515-clone) pin assignments.
 *
 * The board uses SPI1, with the MCP2515 /INT line on GP8. That happens
 * to also be one of SPI1's RX (MISO) options on RP2350, so we use the
 * alternate SPI1_RX pin (GP12) instead. The XL2515 sits on the bus
 * with an 8 MHz crystal; SPI clock runs at 8 MHz (datasheet max for
 * MCP2515 is 10 MHz).
 *
 * Wiring on the board (per Waveshare schematic / community discussions):
 *   GP8  → MCP2515 /INT  (active-low RX interrupt)
 *   GP9  → MCP2515 /CS   (chip select)
 *   GP10 → SPI1_SCK
 *   GP11 → SPI1_TX  (MOSI)
 *   GP12 → SPI1_RX  (MISO)
 */

#include "hardware/spi.h"

#define PIN_SPI_PORT    spi1
#define PIN_SPI_SCK     10
#define PIN_SPI_MOSI    11
#define PIN_SPI_MISO    12
#define PIN_MCP2515_CS  9
#define PIN_MCP2515_INT 8

#define MCP2515_SPI_FREQ_HZ  (8 * 1000 * 1000)

#endif /* PIN_CONFIG_H */

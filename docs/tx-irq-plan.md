# MCP2515 interrupt-driven driver — plan

## Why

Checkpoint 4a's `examples/uds-server/hal/mcp2515.c` works but is
polled-RX + busy-wait-TX. Two real costs:

1. **Hot-loop SPI overhead.** Every iteration of the example's main
   loop does at least one `mcp2515_read_reg(CANINTF)` to ask "any
   frame to grab?". At 500 kbps the bus delivers ~4400 frames/s
   max; on an idle bus we still poll thousands of times per second
   for nothing. SPI traffic + a couple of `gpio_put`s per poll is
   a few hundred CPU cycles burned per loop on a CPU-bound device.

2. **TX serialisation cost.** `mcp2515_send` busy-waits up to ~250 µs
   per frame for TXB0 to clear. For a 5-frame ISO-TP burst that's
   ~1.25 ms of stalled main-loop time. When 4b lands a SUIT envelope
   transfer (hundreds of CFs), the cumulative stall scales linearly.

Neither cost is *yet* a problem for 4a (no SUIT, no second
peripheral). They will be once we layer SUIT-over-UDS and any
concurrent work on the same core. The interrupt-driven driver is
also the cleaner long-term shape for a HAL we want to reuse on
ESP32 and elsewhere.

## What

A drop-in replacement for `mcp2515.c` that:

- Wires `/INT` (currently GP8) to a GPIO falling-edge IRQ handler.
- ISR reads `CANINTF`, drains all set RX FIFOs into a SPSC ring
  buffer, services any TX0IF/TX1IF/TX2IF flags by popping the next
  pending frame from a software TX FIFO and loading it into the
  freed buffer, then clears the serviced flags.
- `mcp2515_receive()` becomes a non-blocking pop from the RX ring.
- `mcp2515_send()` enqueues into the TX FIFO; if TXB0 is idle and
  the FIFO was previously empty, the call also kicks the chip
  directly. Otherwise the next TX0IF triggers the kick.
- Single-TXB on the wire (TXB0 only) — keeps the no-reorder property
  that the busy-wait fix gave us, without blocking.

Module surface stays the same:
`mcp2515_init/send/receive/update_filter`. The example doesn't
change.

## Where it lives

The driver currently lives **per-example** under
`sumo-rp2350/examples/uds-server/hal/mcp2515.{c,h}`. A future
`examples/uds-ota/` (4b/4c) would copy it again, which gets
expensive once we start fixing bugs.

Two paths:

A. **Keep per-example, share the source by hand.** Same as we do for
   `flash.sh` today. Pragmatic but bug-fixes have to be re-ported.

B. **Pull the driver into a shared place.** Either:
   - `sumo-rp2350/hal/` as a CMake INTERFACE/STATIC target the
     examples consume. Works.
   - `uds-tiny/hal/mcp2515/` as the canonical shared XL2515/MCP2515
     binding for any uds-tiny consumer. Better — ESP32-side examples
     of uds-tiny will plausibly want it too (some ESP32 boards put
     an MCP2515 on SPI even though TWAI exists).

   B-via-uds-tiny is the right call once we touch the driver again.

Recommended order: implement IRQ-driven *in place* under
`examples/uds-server/hal/` (same path as today), prove it on bench,
then promote to `uds-tiny/hal/mcp2515/` along with `examples/uds-ota/`
adopting the same path.

## Concrete sketch

```c
/* mcp2515.c (interrupt-driven, single-TXB) */

#define TX_FIFO_DEPTH 16   /* tune for largest expected ISO-TP burst */
static can_frame_t s_tx_fifo[TX_FIFO_DEPTH];
static volatile uint16_t s_tx_head, s_tx_tail;   /* SPSC */

#define RX_FIFO_DEPTH 16
static can_frame_t s_rx_fifo[RX_FIFO_DEPTH];
static volatile uint16_t s_rx_head, s_rx_tail;

static volatile bool s_tx_idle = true;   /* TXB0 free */

static void mcp2515_isr(uint gpio, uint32_t events) {
    /* /INT goes low while ANY enabled flag in CANINTF is set; stay
     * in this loop until everything is serviced. */
    for (;;) {
        uint8_t f = mcp2515_read_reg(REG_CANINTF);
        if ((f & (CANINTF_RX0IF | CANINTF_RX1IF |
                  CANINTF_TX0IF | CANINTF_TX1IF | CANINTF_TX2IF |
                  CANINTF_ERRIF | CANINTF_MERRF)) == 0)
            break;

        if (f & CANINTF_RX0IF) { /* read RXB0 → enqueue → clear flag */ }
        if (f & CANINTF_RX1IF) { /* read RXB1 → enqueue → clear flag */ }
        if (f & CANINTF_TX0IF) {
            mcp2515_clear_flag(CANINTF_TX0IF);
            if (tx_fifo_pop_into_txb0()) {
                spi_kick_rts(0);
            } else {
                s_tx_idle = true;
            }
        }
        /* TX1IF/TX2IF/ERR/MERR likewise */
    }
}

bool mcp2515_send(const can_frame_t *frame) {
    /* Disable IRQ briefly to avoid race on s_tx_idle and FIFO push */
    uint32_t save = save_and_disable_interrupts();
    bool was_idle = s_tx_idle;
    bool ok = tx_fifo_push(frame);
    if (ok && was_idle) {
        write_txb0(frame);  /* same SPI sequence as today */
        spi_kick_rts(0);
        s_tx_idle = false;
        /* don't pop FIFO — we've consumed the slot already */
        tx_fifo_advance_consumer();
    }
    restore_interrupts(save);
    return ok;
}

bool mcp2515_receive(can_frame_t *out) {
    if (s_rx_head == s_rx_tail) return false;
    *out = s_rx_fifo[s_rx_tail % RX_FIFO_DEPTH];
    s_rx_tail++;
    return true;
}

bool mcp2515_init(void) {
    /* … existing init … */
    mcp2515_write_reg(REG_CANINTE,
                      CANINTE_RX0IE | CANINTE_RX1IE |
                      CANINTE_TX0IE | CANINTE_TX1IE | CANINTE_TX2IE);
    gpio_set_irq_enabled_with_callback(
        PIN_MCP2515_INT, GPIO_IRQ_EDGE_FALL, true, mcp2515_isr);
    return true;
}
```

Knobs to size right: `TX_FIFO_DEPTH` and `RX_FIFO_DEPTH`. For 4a
either at 16 is comfortably more than needed; for 4b a SUIT-envelope
download will pump O(100) CFs, but at 500 kbps the consumer side
drains as fast as the producer, so a depth of 32 is plenty.

## Risks / open questions

- **GPIO IRQ vs. mcp2515 /INT level-low semantics.** The MCP2515
  holds /INT low until *all* flags in CANINTF have been cleared. We
  use edge-fall so we don't re-fire on the same low; the ISR loop
  must drain everything in one go (sketch above does this).
- **SPI from ISR context.** pico-sdk's `spi_write_blocking` is
  generally safe from ISR but not officially guaranteed under heavy
  contention. If it bites, move SPI work to a deferred task or onto
  Core 1.
- **Concurrent main-loop + ISR access to RX/TX FIFOs.** SPSC pattern
  + volatile head/tail is sufficient as long as one side only writes
  head and the other only writes tail. ISR pushes RX, main pops RX;
  main pushes TX, ISR pops TX. Standard SPSC. `__sync_synchronize()`
  or `__DMB()` between FIFO write and head increment if reordering
  bites.
- **IRQ cost vs. polling.** RX-frame handler does a few SPI reg
  reads + `memcpy` into the ring + flag clear. Bus at 500 kbps =
  one IRQ every ~250 µs at full load. Cortex-M33 at 150 MHz handles
  that with margin to spare.

## Acceptance

- `examples/uds-server/host/run-test.sh` passes (same as 4a today).
- `candump -td can0` shows ISO-TP CFs in correct sequence under all
  burst sizes the example exercises.
- A microbenchmark prints CPU time spent in `mcp2515_send` per
  frame: target ≤ 30 µs (vs. ~250 µs today).
- A larger smoke test pushes 100+ ISO-TP CFs in a row and confirms
  no frame drop or reorder.

## When

Promote to its own checkpoint (4a-cleanup or before 4b lands).
Doesn't block 4b architecturally — the busy-wait works — but 4b's
SUIT-over-UDS download will make polling cost obvious enough that
it'll pay for itself.

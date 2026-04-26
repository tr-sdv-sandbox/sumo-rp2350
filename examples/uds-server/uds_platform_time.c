/*
 * pico-sdk implementation of uds-tiny's platform time interface.
 *
 * Resolves the extern declarations in
 * uds-tiny/hal/include/uds_platform_time.h at link time.
 */
#include "uds_platform_time.h"
#include "pico/stdlib.h"

uint32_t uds_platform_time_ms(void) {
    return (uint32_t)to_ms_since_boot(get_absolute_time());
}

bool uds_platform_time_expired(uint32_t start_ms, uint32_t timeout_ms) {
    return (uds_platform_time_ms() - start_ms) >= timeout_ms;
}

uint32_t uds_platform_time_elapsed(uint32_t start_ms) {
    return uds_platform_time_ms() - start_ms;
}

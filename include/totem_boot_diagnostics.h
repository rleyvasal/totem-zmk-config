#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void totem_boot_diagnostics_loaded(uint32_t previous_uptime, int8_t previous_profile,
                                   int8_t previous_endpoint);
int totem_boot_diagnostics_persist(void);
void totem_boot_diagnostics_reset_result(int result);
void totem_boot_diagnostics_fault_status(uint8_t status);
void totem_boot_diagnostics_format(char *line, size_t len, bool previous);

#pragma once
#include <stdint.h>
#include <stdbool.h>

void totem_diagnostic_timing_capture(uint32_t uptime_ms, uint8_t type, int8_t idx,
                                      int8_t active, uint8_t reason, uint8_t pair, uint8_t extra);
void totem_handoff_connected(uint16_t handle);
void totem_handoff_disconnected(uint16_t handle);
bool totem_handoff_tx_armed(uint16_t handle);
void totem_handoff_tx_setup(uint16_t handle);
void totem_handoff_tx_result(uint16_t handle, bool completed);
struct ll_conn;
struct node_tx;
void totem_security_tx_queued(struct ll_conn *conn, struct node_tx *tx);

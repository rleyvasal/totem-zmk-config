#pragma once

/* Framed 'B' battery snapshot on the Studio CDC. Independent of Enable log.
 * Returns 0 if queued to the host, negative if the cable/DTR is not ready. */
int totem_studio_send_battery(const char *line);

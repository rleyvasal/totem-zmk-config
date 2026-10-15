#pragma once

/* Framed 'K' battery snapshot on the Studio CDC (via zmk-studio-log). Independent of
 * Enable log. Returns 0 if queued to the host, negative if USB is not ready. */
int totem_studio_send_battery(const char *line);

/* Send a 'K' battery snapshot now instead of at the next 10 s tick. Used to answer a
 * host's 'G' '1' request, so a battery app can open the port, ask, and close quickly. */
void totem_battery_send_now(void);

/* Bypass the lossy live printk queue for a numbered diagnostic dump line. */
int totem_studio_send_diag_line(const char *line);

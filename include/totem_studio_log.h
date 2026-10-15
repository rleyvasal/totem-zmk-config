#pragma once

/* Framed 'K' battery snapshot on the Studio CDC (via zmk-studio-log). Independent of
 * Enable log. Returns 0 if queued to the host, negative if USB is not ready. */
int totem_studio_send_battery(const char *line);

/* Bypass the lossy live printk queue for a numbered diagnostic dump line. */
int totem_studio_send_diag_line(const char *line);

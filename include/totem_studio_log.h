#pragma once

/* Bypass the lossy live printk queue for a numbered diagnostic dump line. */
int totem_studio_send_diag_line(const char *line);

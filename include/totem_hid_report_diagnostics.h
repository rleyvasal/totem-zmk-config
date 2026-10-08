#pragma once

#include <stdbool.h>

struct bt_conn;
/* Called on the HOG worker immediately before its keyboard notification. */
void totem_hid_report_observed(struct bt_conn *conn, bool subscribed);

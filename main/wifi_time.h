#ifndef WIFI_TIME_H
#define WIFI_TIME_H

#include <stdbool.h>
#include <stddef.h>

bool wifi_time_init(void);

/*
 * Fills three separate strings for the LCD:
 *   day_out  -> English weekday name, uppercase, e.g. "THURSDAY"
 *   time_out -> "HH : MM : SS"
 *   date_out -> "DD/MM/YYYY"
 * Returns false (and fills placeholder dashes in all three buffers) if the
 * system clock has not been synchronized via NTP yet.
 */
bool wifi_time_get_display(char *day_out, size_t day_out_sz,
                            char *time_out, size_t time_out_sz,
                            char *date_out, size_t date_out_sz);

#endif
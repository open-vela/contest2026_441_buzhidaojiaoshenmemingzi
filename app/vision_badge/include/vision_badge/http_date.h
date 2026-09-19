/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONTEST2026_441_VISION_BADGE_HTTP_DATE_H
#define CONTEST2026_441_VISION_BADGE_HTTP_DATE_H
#include <time.h>
int vision_http_date_parse(const char *headers, time_t *epoch);
#endif
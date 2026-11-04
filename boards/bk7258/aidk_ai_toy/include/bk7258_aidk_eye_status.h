/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BK7258_AIDK_EYE_STATUS_H
#define BK7258_AIDK_EYE_STATUS_H
#include <stdint.h>
#define AIDK_STATUS_WIDTH 160u
#define AIDK_STATUS_HEIGHT 24u
/* Called exclusively by the existing eye worker, never by RPC/audio tasks.
 * State values follow bk7258_aidk_eye_state_e. No panel I/O in the renderer. */
int bk7258_aidk_eye_status_render(unsigned int state);
const uint16_t *bk7258_aidk_eye_status_row(unsigned int row);
#endif

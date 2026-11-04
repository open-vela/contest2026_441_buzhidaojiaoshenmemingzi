/* SPDX-License-Identifier: Apache-2.0 */
#ifndef VISION_BADGE_XIAOZHI_CONVERSATION_H
#define VISION_BADGE_XIAOZHI_CONVERSATION_H
#include <stddef.h>
/* Commands return immediately. Only the background owner joins workers and
 * releases audio/network/codec state. config is saved on AP SD-NAND.
 */
int xiaozhi_conversation_command(const char *action, char *summary, size_t size);
#endif

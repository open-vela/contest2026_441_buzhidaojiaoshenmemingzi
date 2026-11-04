/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BK7258_AOSL_NUTTX_COMPAT_H
#define BK7258_AOSL_NUTTX_COMPAT_H

/* Only preincluded in portable AOSL translation units, never in NuttX or
 * the native HAL. Parse OS headers with the OS atomic namespace intact.
 * AOSL uses an intptr_t counter, not NuttX's compiler-atomic type, and its
 * add/sub arguments have the opposite order. Do not mix the two APIs.
 */

#include <sys/types.h>
#include <nuttx/fs/fs.h>
#include <nuttx/mutex.h>
#include <nuttx/spinlock.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>

#undef atomic_read
#undef atomic_set
#undef atomic_add
#undef atomic_sub
#undef atomic_inc
#undef atomic_dec
#undef atomic_add_return
#undef atomic_sub_return
#undef atomic_inc_return
#undef atomic_dec_return
#undef atomic_inc_and_test
#undef atomic_dec_and_test
#undef atomic_cmpxchg
#undef atomic_xchg
#define atomic_t aosl_private_atomic_t

#ifndef __always_inline
#  define __always_inline inline __attribute__((__always_inline__))
#endif

#endif

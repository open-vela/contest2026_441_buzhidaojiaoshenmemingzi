/* SPDX-License-Identifier: Apache-2.0 */
#ifndef R1_TEST_SESSION_MUTEX_H
#define R1_TEST_SESSION_MUTEX_H
#include <pthread.h>
typedef pthread_mutex_t mutex_t;
#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
extern _Thread_local int r1_test_lock_depth;
static inline int nxmutex_lock(mutex_t *lock)
{
  int ret = pthread_mutex_lock(lock);
  if (!ret) r1_test_lock_depth++;
  return -ret;
}
static inline int nxmutex_unlock(mutex_t *lock)
{
  r1_test_lock_depth--;
  return -pthread_mutex_unlock(lock);
}
#endif

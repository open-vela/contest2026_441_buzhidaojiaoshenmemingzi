/* SPDX-License-Identifier: Apache-2.0 */
#include <pthread.h>
typedef pthread_mutex_t mutex_t;
#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
static inline int nxmutex_init(mutex_t *lock)
{
  return -pthread_mutex_init(lock, NULL);
}
static inline int nxmutex_destroy(mutex_t *lock)
{
  return -pthread_mutex_destroy(lock);
}
static inline int nxmutex_lock(mutex_t *lock)
{
  return -pthread_mutex_lock(lock);
}
static inline int nxmutex_trylock(mutex_t *lock)
{
  return -pthread_mutex_trylock(lock);
}
static inline int nxmutex_unlock(mutex_t *lock)
{
  return -pthread_mutex_unlock(lock);
}

/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise the pinned, unmodified AOSL embedded queue path with our HAL.
 * This is a Linux-host integration test, not execution of ARM RTC code. */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include <api/aosl_mpq.h>
#include <api/aosl_ref.h>
#include <api/aosl_errno.h>

extern void aosl_ctor(void);
extern void aosl_dtor(void);
static atomic_int initialized, finalized, called;
static int on_init(void *arg)
{
  (void)arg;
  assert(!aosl_mpq_invalid(aosl_mpq_this()));
  atomic_fetch_add(&initialized, 1);
  return 0;
}
static void on_fini(void *arg)
{ (void)arg; atomic_fetch_add(&finalized, 1); }
static void on_call(const uintptr_t *ref, void *object, uintptr_t argc, uintptr_t argv[])
{
  (void)ref; (void)object;
  assert(argc == 1 && argv[0] == 123);
  assert(!aosl_mpq_invalid(aosl_mpq_this()));
  atomic_fetch_add(&called, 1);
}
int main(void)
{
  for (int round = 0; round < 2; round++)
    {
      aosl_ctor();
      aosl_mpq_t q = aosl_mpq_create(AOSL_THRD_PRI_HIGH, 32768, 10000,
                                    "AgoraRtcTest", on_init, on_fini, NULL);
      if (aosl_mpq_invalid(q))
        { fprintf(stderr, "MPQ creation failed, AOSL errno=%d\n", aosl_errno); return 1; }
      aosl_mpq_t callback_q = aosl_mpq_create_flags(AOSL_MPQ_FLAG_SIGP_EVENT,
                         AOSL_THRD_PRI_HIGH, 6144, 10000, "AgoraRtcCbTest",
                         on_init, on_fini, NULL);
      assert(!aosl_mpq_invalid(callback_q));
      /* Let select block, then wake it through actual os_socket_pipe UDP. */
      usleep(100000);
      assert(aosl_mpq_call(q, AOSL_REF_INVALID, "test", on_call, 1,
                           (uintptr_t)123) == 0);
      assert(aosl_mpq_call(callback_q, AOSL_REF_INVALID, "callback", on_call, 1,
                           (uintptr_t)123) == 0);
      for (int wait = 0; wait < 2000 && atomic_load(&called) != (round + 1) * 2; wait++)
        usleep(1000);
      assert(atomic_load(&called) == (round + 1) * 2);
      assert(aosl_mpq_destroy_wait(callback_q) == 0);
      assert(aosl_mpq_destroy_wait(q) == 0);
      assert(atomic_load(&initialized) == (round + 1) * 2);
      assert(atomic_load(&finalized) == (round + 1) * 2);
      aosl_dtor();
    }
  puts("Pinned AOSL embedded path PASS: ctor, UDP/select and event queues, TLS callbacks, small-stack request, destroy and re-entry; no ARM RTC/audio claim");
  return 0;
}

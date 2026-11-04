/* SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#ifdef VISION_AGORA_CONNECT_TEST
#include <sys/select.h>
#include <sys/syscall.h>
static int connect_interrupts, connect_select_calls;
static long connect_remaining_us;
int r1_test_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *timeout)
{
  if (connect_interrupts)
    {
      assert(timeout && w);
      long remaining = timeout->tv_sec * 1000000L + timeout->tv_usec;
      assert(remaining > 0 && remaining <= connect_remaining_us);
      connect_remaining_us = remaining;
      connect_select_calls++;
      connect_interrupts--;
      /* select sets are not guaranteed intact when interrupted. */
      FD_ZERO(w);
      usleep(20000);
      errno = EINTR;
      return -1;
    }
  return syscall(SYS_select, n, r, w, e, timeout);
}
#endif
#ifdef VISION_AGORA_NET_TEST
#include <sys/syscall.h>
static int write_error;
static int sendto_error, sendto_calls;
static int test_iobs = 36;
int r1_test_iob_navail(void)
{
  /* Deliberately dirty errno: diagnostics must preserve the socket result. */
  errno = EIO;
  return test_iobs;
}
ssize_t r1_test_sendto(int fd, const void *buffer, size_t count, int flags,
                      const struct sockaddr *address, socklen_t size)
{
  sendto_calls++;
  if (sendto_error) { errno = sendto_error; return -1; }
  return syscall(SYS_sendto, fd, buffer, count, flags, address, size);
}
ssize_t r1_test_write(int fd, const void *buffer, size_t count)
{
  if (write_error) { errno = write_error; return -1; }
  return syscall(SYS_write, fd, buffer, count);
}
#endif
extern void bk7258_agora_netreset(void);
extern void bk7258_agora_netstats(char *text, size_t capacity);
#ifdef VISION_AGORA_SCHED_TEST
static int attr_priority, attr_calls, attr_error;
int r1_test_attr_inherit(pthread_attr_t *attr, int inherit)
{ (void)attr; assert(inherit == PTHREAD_EXPLICIT_SCHED); return 0; }
int r1_test_attr_policy(pthread_attr_t *attr, int policy)
{ (void)attr; assert(policy == SCHED_RR); return 0; }
int r1_test_attr_priority(pthread_attr_t *attr, const struct sched_param *param)
{
  (void)attr;
  attr_calls++; attr_priority = param->sched_priority;
  return attr_error;
}
static void *no_work(void *arg) { return arg; }
#endif
#include <arpa/inet.h>
#include <hal/aosl_hal_atomic.h>
#include <hal/aosl_hal_thread.h>
#include <hal/aosl_hal_memory.h>
#include <hal/aosl_hal_time.h>
#include <hal/aosl_hal_socket.h>
#include <hal/aosl_hal_errno.h>
#include <hal/aosl_hal_iomp.h>
#include <hal/aosl_hal_file.h>
#include <hal/aosl_hal_utils.h>
static intptr_t counter;
static void interrupted(int signal_number) { (void)signal_number; }
static void *increment(void *p)
{
  for (int i = 0; i < 10000; i++) aosl_hal_atomic_inc(&counter);
  return p;
}
static void sockets(int family, const char *host)
{
  aosl_sockaddr_t resolved[4] = {0}, local = {0}, source = {0};
  int found = aosl_hal_gethostbyname(host, resolved, 4);
  assert(found > 0);
  assert(resolved[0].sa_family == family);
  int fd = aosl_hal_sk_socket(family, AOSL_SOCK_DGRAM, 0);
  assert(fd >= 0);
  assert(aosl_hal_sk_bind(fd, &resolved[0]) == 0);
  assert(aosl_hal_sk_get_sockname(fd, &local) == 0);
  assert(local.sa_family == family && local.sa_port != 0);
  int tx = aosl_hal_sk_socket(family, AOSL_SOCK_DGRAM, 0);
  assert(tx >= 0);
  /* Same connected UDP loopback as AOSL os_socket_pipe(), not just sendto. */
  assert(aosl_hal_sk_connect(tx, &local) == 0);
  assert(aosl_hal_sk_send(tx, "hello", 5, 0) == 5);
  fd_set_t readset = aosl_hal_fdset_create();
  assert(readset);
  aosl_hal_fdset_zero(readset); aosl_hal_fdset_set(readset, fd);
  aosl_hal_fdset_set(readset, -1); aosl_hal_fdset_set(readset, INT_MAX);
  assert(aosl_hal_select(fd + 1, readset, NULL, NULL, 1000) == 1);
  char buf[16];
  assert(aosl_hal_sk_recvfrom(fd, buf, sizeof(buf), 0, &source) == 5);
  assert(!memcmp(buf, "hello", 5)); assert(source.sa_family == family);
  assert(aosl_hal_sk_set_nonblock(fd) == 0);
  assert(aosl_hal_sk_recv(fd, buf, sizeof(buf), 0) == AOSL_HAL_RET_EAGAIN);
  aosl_hal_fdset_destroy(readset);
  assert(aosl_hal_sk_close(fd) == 0); assert(aosl_hal_sk_close(tx) == 0);
}
static void udp_frame_payloads(void)
{
  aosl_sockaddr_t address[1] = {0}, bound = {0}, source = {0};
  const size_t lengths[] = {548, 549, 1200, 1472};
  /* Cross the old MTU's UDP limit (590 - Ethernet/IP/UDP headers).
   * Linux loopback verifies the adapter preserves each complete datagram;
   * it is not proof that the board can transmit it over Wi-Fi. */
  uint8_t tx[1472], rx[1472];
  for (size_t i = 0; i < sizeof(tx); i++) tx[i] = (uint8_t)(i * 37u);
  assert(aosl_hal_gethostbyname("127.0.0.1", address, 1) == 1);
  int receiver = aosl_hal_sk_socket(AOSL_AF_INET, AOSL_SOCK_DGRAM, 0);
  int sender = aosl_hal_sk_socket(AOSL_AF_INET, AOSL_SOCK_DGRAM, 0);
  assert(receiver >= 0 && sender >= 0);
  assert(aosl_hal_sk_bind(receiver, address) == 0);
  assert(aosl_hal_sk_get_sockname(receiver, &bound) == 0);
  fd_set_t ready = aosl_hal_fdset_create();
  assert(ready);
  for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
    {
      assert(aosl_hal_sk_sendto(sender, tx, lengths[i], 0, &bound) == (int)lengths[i]);
      aosl_hal_fdset_zero(ready); aosl_hal_fdset_set(ready, receiver);
      assert(aosl_hal_select(receiver + 1, ready, NULL, NULL, 1000) == 1);
      memset(rx, 0, sizeof(rx));
      assert(aosl_hal_sk_recvfrom(receiver, rx, sizeof(rx), 0, &source) == (int)lengths[i]);
      assert(memcmp(tx, rx, lengths[i]) == 0);
    }
  uint8_t *oversized = calloc(65536, 1);
  assert(oversized);
  assert(aosl_hal_sk_sendto(sender, oversized, 65536, 0, &bound) == aosl_hal_errno_convert(EMSGSIZE));
  assert(errno == EMSGSIZE); /* Diagnostics must preserve the real error. */
  free(oversized);
  aosl_hal_fdset_destroy(ready);
  assert(aosl_hal_sk_close(sender) == 0);
  assert(aosl_hal_sk_close(receiver) == 0);
}
#ifdef VISION_AGORA_NET_TEST
static void net_diagnostics(void)
{
  aosl_sockaddr_t address[1] = {0}, bound = {0};
  char text[256], received[8];
  assert(aosl_hal_gethostbyname("127.0.0.1", address, 1) == 1);
  int rx = aosl_hal_sk_socket(AOSL_AF_INET, AOSL_SOCK_DGRAM, 0);
  int tx = aosl_hal_sk_socket(AOSL_AF_INET, AOSL_SOCK_DGRAM, 0);
  assert(rx >= 0 && tx >= 0);
  assert(aosl_hal_sk_bind(rx, address) == 0);
  assert(aosl_hal_sk_get_sockname(rx, &bound) == 0);
  assert(aosl_hal_sk_connect(tx, &bound) == 0);
  bk7258_agora_netreset();
  assert(aosl_hal_sk_set_nonblock(rx) == 0);
  test_iobs = 0;
  assert(aosl_hal_sk_read(rx, received, sizeof(received)) == AOSL_HAL_RET_EAGAIN);
  bk7258_agora_netstats(text, sizeof(text));
  assert(strstr(text, "tx_again=0 iob_start/now/fail_min=36/0/-1"));
  test_iobs = 36;
  assert(aosl_hal_sk_write(tx, "check", 5) == 5);
  assert(aosl_hal_sk_read(rx, received, sizeof(received)) == 5);
  write_error = ENOMEM;
  test_iobs = 2;
  assert(aosl_hal_sk_write(tx, "check", 5) == aosl_hal_errno_convert(ENOMEM));
  assert(errno == ENOMEM);
  bk7258_agora_netstats(text, sizeof(text));
  assert(strstr(text, "socket_tx/rx_bytes=5/5 calls=1/1 enomem=1 other_fail=0"));
  assert(strstr(text, "last_errno=12 type=2 peer_loop=1"));
  assert(strstr(text, "tx_again=0 iob_start/now/fail_min=36/2/2"));
  assert(errno == ENOMEM);
  assert(strlen(text) < sizeof(text) - 1);
  write_error = EAGAIN;
  test_iobs = 0;
  assert(aosl_hal_sk_write(tx, "check", 5) == AOSL_HAL_RET_EAGAIN && errno == EAGAIN);
  bk7258_agora_netstats(text, sizeof(text));
  assert(strstr(text, "enomem=1 other_fail=0"));
  assert(strstr(text, "tx_again=1 iob_start/now/fail_min=36/0/0"));
  test_iobs = 35;
  bk7258_agora_netstats(text, sizeof(text));
  assert(strstr(text, "iob_start/now/fail_min=36/35/0"));
  assert(errno == EAGAIN);
  char tiny[4] = {0};
  bk7258_agora_netstats(tiny, sizeof(tiny));
  assert(tiny[3] == '\0');
  write_error = 0;
  assert(aosl_hal_sk_close(tx) == 0 && aosl_hal_sk_close(rx) == 0);
  bk7258_agora_netreset();
  assert(aosl_hal_sk_write(-1, "check", 5) == AOSL_HAL_RET_FAILURE && errno == EBADF);
  bk7258_agora_netstats(text, sizeof(text));
  assert(strstr(text, "socket_tx/rx_bytes=0/0 calls=0/0 enomem=0 other_fail=1"));
  assert(strstr(text, "type=0 peer_loop=-1"));
  assert(strstr(text, "tx_again=0 iob_start/now/fail_min=35/35/-1"));
  test_iobs = -1;
  bk7258_agora_netreset();
  write_error = ENOMEM;
  assert(aosl_hal_sk_write(-1, "check", 5) == aosl_hal_errno_convert(ENOMEM));
  bk7258_agora_netstats(text, sizeof(text));
  assert(strstr(text, "iob_start/now/fail_min=-1/-1/-1"));
  write_error = 0;
  test_iobs = 36;
  puts("Agora IOB snapshots PASS: low/exhausted/recovered/reset/unavailable, errno and bounded output; pool mocked");
  puts("Agora socket diagnostics PASS: real UDP I/O, injected ENOMEM/EAGAIN, errno preservation, unknown peer, bounded counters/reset; allocation failure mocked");
}

static void sendto_diagnostics(void)
{
  const char *hosts[] = {"127.0.0.1", "192.0.2.1", "::1", "2001:db8::1"};
  for (unsigned int i = 0; i < 4; i++)
    {
      aosl_sockaddr_t address = {0};
      char text[256];
      address.sa_family = i < 2 ? AOSL_AF_INET : AOSL_AF_INET6;
      address.sa_port = htons(9000);
      assert(inet_pton(i < 2 ? AF_INET : AF_INET6, hosts[i],
                      i < 2 ? (void *)&address.sin_addr : (void *)address.sin6_addr) == 1);
      int fd = aosl_hal_sk_socket(address.sa_family, AOSL_SOCK_DGRAM, 0);
      assert(fd >= 0); /* Deliberately unconnected, like RTC sendto. */
      bk7258_agora_netreset();
      sendto_error = ENOMEM;
      sendto_calls = 0;
      assert(aosl_hal_sk_sendto(fd, "check", 5, 0, &address) == aosl_hal_errno_convert(ENOMEM));
      assert(errno == ENOMEM && sendto_calls == 1); /* No hidden retry. */
      bk7258_agora_netstats(text, sizeof(text));
      assert(strstr(text, "socket_tx/rx_bytes=0/0 calls=0/0 enomem=1 other_fail=0"));
      assert(strstr(text, i % 2 == 0 ? "peer_loop=1" : "peer_loop=0"));
      assert(strlen(text) < sizeof(text) - 1);
      sendto_error = EAGAIN;
      assert(aosl_hal_sk_sendto(fd, "check", 5, 0, &address) == AOSL_HAL_RET_EAGAIN);
      assert(errno == EAGAIN && sendto_calls == 2);
      bk7258_agora_netstats(text, sizeof(text));
      assert(strstr(text, "enomem=1 other_fail=0"));
      sendto_error = 0;
      assert(aosl_hal_sk_close(fd) == 0);
    }
  puts("Agora sendto diagnostics PASS: unconnected IPv4/IPv6 loopback/non-loopback, preserved ENOMEM/EAGAIN, no retry; sendto failure injected, no external traffic");
}
#endif
static void tcp_connect(void)
{
  aosl_sockaddr_t address[1] = {0}, bound = {0};
  assert(aosl_hal_gethostbyname("127.0.0.1", address, 1) == 1);
  int listener = aosl_hal_sk_socket(AOSL_AF_INET, AOSL_SOCK_STREAM, 0);
  assert(listener >= 0);
  assert(aosl_hal_sk_bind(listener, address) == 0);
  assert(aosl_hal_sk_get_sockname(listener, &bound) == 0);
  assert(aosl_hal_sk_listen(listener, 1) == 0);
  int client = aosl_hal_sk_socket(AOSL_AF_INET, AOSL_SOCK_STREAM, AOSL_IPPROTO_TCP);
  assert(client >= 0 && aosl_hal_sk_set_nonblock(client) == 0);
  assert(aosl_hal_sk_connect(client, &bound) == 0);
  int accepted = aosl_hal_sk_accept(listener, NULL);
  assert(accepted >= 0);
  assert(aosl_hal_sk_send(client, "ok", 2, 0) == 2);
  char data[2];
  assert(aosl_hal_sk_recv(accepted, data, sizeof(data), 0) == 2);
  assert(!memcmp(data, "ok", 2));
  assert(aosl_hal_sk_close(accepted) == 0);
  assert(aosl_hal_sk_close(client) == 0);
  /* A bound, non-listening socket reserves the port without accepting TCP. */
  assert(aosl_hal_sk_close(listener) == 0);
  listener = aosl_hal_sk_socket(AOSL_AF_INET, AOSL_SOCK_STREAM, AOSL_IPPROTO_TCP);
  assert(listener >= 0 && aosl_hal_sk_bind(listener, address) == 0);
  assert(aosl_hal_sk_get_sockname(listener, &bound) == 0);
  client = aosl_hal_sk_socket(AOSL_AF_INET, AOSL_SOCK_STREAM, AOSL_IPPROTO_TCP);
  assert(client >= 0 && aosl_hal_sk_set_nonblock(client) == 0);
  assert(aosl_hal_sk_connect(client, &bound) < 0 && errno == ECONNREFUSED);
  assert(aosl_hal_sk_close(client) == 0);
  assert(aosl_hal_sk_close(listener) == 0);
}
#ifdef VISION_AGORA_CONNECT_TEST
static void interrupted_connect(void)
{
  for (int timeout_case = 0; timeout_case < 2; timeout_case++)
    {
      aosl_sockaddr_t address[1] = {0}, bound = {0};
      assert(aosl_hal_gethostbyname("127.0.0.1", address, 1) == 1);
      int listener = aosl_hal_sk_socket(AOSL_AF_INET, AOSL_SOCK_STREAM, 0);
      assert(listener >= 0 && aosl_hal_sk_bind(listener, address) == 0);
      assert(aosl_hal_sk_get_sockname(listener, &bound) == 0);
      assert(aosl_hal_sk_listen(listener, 1) == 0);
      int client = aosl_hal_sk_socket(AOSL_AF_INET, AOSL_SOCK_STREAM, 0);
      assert(client >= 0 && aosl_hal_sk_set_nonblock(client) == 0);
      connect_interrupts = timeout_case ? 100 : 2;
      connect_select_calls = 0; connect_remaining_us = LONG_MAX;
      uint64_t begin = aosl_hal_get_tick_ms();
      int ret = aosl_hal_sk_connect(client, &bound);
      int error = errno;
      uint64_t elapsed = aosl_hal_get_tick_ms() - begin;
      if (timeout_case)
        {
          assert(ret == AOSL_HAL_RET_FAILURE && error == ETIMEDOUT);
          assert(connect_select_calls >= 3 && connect_select_calls <= 5);
          assert(elapsed >= 70 && elapsed < 500);
        }
      else
        {
          assert(ret == 0 && connect_select_calls == 2);
          int peer = aosl_hal_sk_accept(listener, NULL); assert(peer >= 0);
          assert(aosl_hal_sk_send(client, "ok", 2, 0) == 2);
          char data[2];
          assert(aosl_hal_sk_recv(peer, data, sizeof(data), 0) == 2);
          assert(!memcmp(data, "ok", 2));
          assert(aosl_hal_sk_close(peer) == 0);
        }
      connect_interrupts = 0;
      assert(aosl_hal_sk_close(client) == 0 && aosl_hal_sk_close(listener) == 0);
    }
  puts("Agora interrupted TCP connect PASS: restored sets, shrinking monotonic budget, timeout and real I/O; interruptions injected");
}
#endif
int main(void)
{
  aosl_thread_param_t p = {.name="hal-test", .stack_size=32768};
  aosl_thread_t a, b;
  void *value;
  assert(aosl_hal_thread_create(&a, &p, increment, (void *)123) == 0);
  assert(aosl_hal_thread_create(&b, &p, increment, (void *)456) == 0);
  assert(aosl_hal_thread_join(a, &value) == 0 && value == (void *)123);
  assert(aosl_hal_thread_join(b, &value) == 0 && value == (void *)456);
  assert(aosl_hal_atomic_read(&counter) == 20000);
  assert(aosl_hal_atomic_cmpxchg(&counter, 20000, 42) == 20000);
  assert(aosl_hal_atomic_cmpxchg(&counter, 43, 99) == 42);
  assert(aosl_hal_atomic_xchg(&counter, 0) == 42);
  p.stack_size = 4096; /* Actual AOSL LTWP request; platform floor applies. */
  assert(aosl_hal_thread_create(&a, &p, increment, (void *)789) == 0);
  assert(aosl_hal_thread_join(a, &value) == 0 && value == (void *)789);
  p.stack_size = -1;
  assert(aosl_hal_thread_create(&a, &p, increment, NULL) == -EINVAL);
  aosl_mutex_t mutex = aosl_hal_mutex_create();
  aosl_cond_t cond = aosl_hal_cond_create();
  assert(mutex && cond);
  assert(aosl_hal_mutex_lock(mutex) == 0);
  uint64_t start = aosl_hal_get_tick_ms();
  assert(aosl_hal_cond_timedwait(cond, mutex, 40) == -ETIMEDOUT);
  uint64_t elapsed = aosl_hal_get_tick_ms() - start;
  assert(elapsed >= 30 && elapsed < 1000);
  assert(aosl_hal_mutex_unlock(mutex) == 0);
  aosl_hal_cond_destroy(cond); aosl_hal_mutex_destroy(mutex);
  assert(aosl_hal_calloc(SIZE_MAX, 2) == NULL);
  char *mem = aosl_hal_malloc(32); assert(mem);
  memset(mem, 0x5a, 32); mem = aosl_hal_realloc(mem, 64); assert(mem);
  for (int i = 0; i < 32; i++) assert(mem[i] == 0x5a);
  aosl_hal_free(mem);
  /* The actual SDK has requested a 409600-byte pool. Exercise that scale,
   * grow/shrink copy preservation and the same realloc/free pairing.
   * Host malloc is not proof of the board's available PSRAM capacity.
   */
  mem = aosl_hal_calloc(409600, 1); assert(mem);
  assert((uintptr_t)mem % 8 == 0);
  for (size_t i = 0; i < 409600; i++) assert(mem[i] == 0);
  memset(mem, 0x5a, 409600);
  mem = aosl_hal_realloc(mem, 819200); assert(mem);
  for (size_t i = 0; i < 409600; i++) assert(mem[i] == 0x5a);
  mem = aosl_hal_realloc(mem, 409600); assert(mem);
  assert(mem[409599] == 0x5a);
  assert(aosl_hal_realloc(mem, 0) == NULL);
  char dir[] = "/tmp/r1-agora-hal-XXXXXX", path[128], renamed[128];
  assert(mkdtemp(dir));
  snprintf(path, sizeof(path), "%s/test.bin", dir);
  snprintf(renamed, sizeof(renamed), "%s/new.bin", dir);
  assert(aosl_hal_file_create(path) == 0);
  aosl_fs_t f = aosl_hal_fopen(path, "wb"); assert(f);
  assert(aosl_hal_fwrite(f, "hello", 5) == 5); assert(aosl_hal_fclose(f) == 0);
  assert(aosl_hal_fsize(path) == 5);
  f = aosl_hal_fopen(path, "rb"); assert(f);
  char buf[32] = {0}; assert(aosl_hal_fread(f, buf, sizeof(buf)) == 5);
  assert(!strcmp(buf, "hello")); assert(aosl_hal_fclose(f) == 0);
  assert(aosl_hal_file_rename(path, renamed) == 0);
  assert(!aosl_hal_fexist(path) && aosl_hal_fexist(renamed));
  assert(aosl_hal_file_delete(renamed) == 0); assert(aosl_hal_rmdir(dir) == 0);
  assert(aosl_hal_get_uuid(buf, sizeof(buf)) == -EINVAL);
  char uuid[33]; assert(aosl_hal_get_uuid(uuid, sizeof(uuid)) == 0);
  assert(strlen(uuid) == 32);
  sockets(AOSL_AF_INET, "127.0.0.1"); sockets(AOSL_AF_INET6, "::1");
  udp_frame_payloads();
  assert(aosl_hal_sk_socket(AOSL_AF_INET, AOSL_SOCK_DGRAM, AOSL_IPPROTO_TCP) == -1 && errno == EINVAL);
  assert(aosl_hal_sk_socket(AOSL_AF_INET, AOSL_SOCK_STREAM, AOSL_IPPROTO_UDP) == -1 && errno == EINVAL);
  tcp_connect();
#ifdef VISION_AGORA_CONNECT_TEST
  interrupted_connect();
#endif
  /* Actual AOSL select_mp.c distinguishes its EINTR sentinel from failure.
   * An OS -1 must therefore be translated, not returned unmodified.
   */
  struct sigaction action = {0};
  action.sa_handler = interrupted;
  sigemptyset(&action.sa_mask);
  assert(sigaction(SIGALRM, &action, NULL) == 0);
  ualarm(20000, 0);
  assert(aosl_hal_select(0, NULL, NULL, NULL, 1000) == AOSL_HAL_RET_EINTR);
  ualarm(0, 0);
  assert(aosl_hal_gethostbyname(NULL, NULL, 0) == 0);
#ifdef VISION_AGORA_NET_TEST
  net_diagnostics();
  sendto_diagnostics();
#endif
#ifdef VISION_AGORA_SCHED_TEST
  const int expected[] = {100, 95, 100, 105, 110, 115};
  p.stack_size = 4096;
  for (unsigned int i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
    {
      p.priority = (aosl_thread_proiority_e)i;
      int before = attr_calls;
      assert(aosl_hal_thread_create(&a, &p, no_work, NULL) == 0);
      assert(attr_calls == before + 1 && attr_priority == expected[i]);
      assert(aosl_hal_thread_join(a, NULL) == 0);
    }
  assert(aosl_hal_thread_create(&a, NULL, no_work, NULL) == 0);
  assert(attr_priority == 100);
  assert(aosl_hal_thread_join(a, NULL) == 0);
  attr_error = EPERM;
  assert(aosl_hal_thread_create(&a, NULL, no_work, NULL) == -EPERM);
  puts("Agora SDK scheduling PASS: explicit mapped/default/NULL priorities, attribute failure preserved; scheduler mocked");
#endif
  puts("Agora NuttX HAL host tests PASS: threads/join, monotonic timeout, atomic counters, files, IPv4/IPv6 UDP/select, interrupted select mapping");
  return 0;
}

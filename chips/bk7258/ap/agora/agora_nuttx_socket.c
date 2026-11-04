/* SPDX-License-Identifier: Apache-2.0 */
/* Chip-owned network adapter for the fixed Agora SDK. */
#ifndef VISION_AGORA_HOST_TEST
#  include <nuttx/config.h>
#  include <nuttx/mm/iob.h>
#endif
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <time.h>
#include <net/if.h>
#include <hal/aosl_hal_socket.h>
#include <hal/aosl_hal_errno.h>

#ifndef AGORA_CONNECT_MS
#  define AGORA_CONNECT_MS 5000u
#endif

/* Bounded, payload-free diagnostics: connection-lost alone does not tell
 * DNS failure from descriptor exhaustion or socket I/O failure. */
static unsigned int g_net_errors;
static unsigned int g_dns_reports;
static unsigned int g_datagram_reports;
static unsigned int g_io[7]; /* tx/rx bytes/calls, ENOMEM, other failures, tx EAGAIN */
static int g_last_type, g_last_loop = -1, g_last_errno;
static int g_iob_start = -1, g_iob_failure_min = -1;

/* Read the AP's pool, not CP's /proc/iobinfo. No allocation or per-packet
 * logging here: SYSLOG_BUFFER itself consumes the same IOB pool.
 * This is a post-return snapshot, not proof of the allocation failure site.
 */
static int available_iobs(void)
{
#ifdef VISION_AGORA_NET_TEST
  extern int r1_test_iob_navail(void);
  return r1_test_iob_navail();
#elif !defined(VISION_AGORA_HOST_TEST) && defined(CONFIG_MM_IOB)
  return iob_navail(false);
#else
  return -1;
#endif
}

static void sample_failed_io(void)
{
  int available = available_iobs();
  int minimum = __atomic_load_n(&g_iob_failure_min, __ATOMIC_RELAXED);
  while (available >= 0 && (minimum < 0 || available < minimum))
    {
      if (__atomic_compare_exchange_n(&g_iob_failure_min, &minimum, available,
                                      false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        break;
    }
}

void bk7258_agora_netreset(void)
{
  int saved = errno;
  for (unsigned int i = 0; i < 7; i++) __atomic_store_n(&g_io[i], 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_iob_start, available_iobs(), __ATOMIC_RELAXED);
  __atomic_store_n(&g_iob_failure_min, -1, __ATOMIC_RELAXED);
  __atomic_store_n(&g_net_errors, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_datagram_reports, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_dns_reports, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_last_errno, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_last_type, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_last_loop, -1, __ATOMIC_RELAXED);
  errno = saved;
}

void bk7258_agora_netstats(char *text, size_t capacity)
{
  int saved = errno;
  int available = available_iobs();
  unsigned int v[7];
  for (unsigned int i = 0; i < 7; i++) v[i] = __atomic_load_n(&g_io[i], __ATOMIC_RELAXED);
  snprintf(text, capacity,
    "Agora net socket_tx/rx_bytes=%u/%u calls=%u/%u enomem=%u other_fail=%u "
    "last_errno=%d type=%d peer_loop=%d tx_again=%u iob_start/now/fail_min=%d/%d/%d",
    v[0], v[1], v[2], v[3], v[4], v[5],
    __atomic_load_n(&g_last_errno, __ATOMIC_RELAXED),
    __atomic_load_n(&g_last_type, __ATOMIC_RELAXED),
    __atomic_load_n(&g_last_loop, __ATOMIC_RELAXED), v[6],
    __atomic_load_n(&g_iob_start, __ATOMIC_RELAXED), available,
    __atomic_load_n(&g_iob_failure_min, __ATOMIC_RELAXED));
  errno = saved;
}

static void io_diagnostic(int fd, size_t requested, ssize_t value, bool transmit,
                          const struct sockaddr_storage *destination)
{
  int saved = errno;
  if (value < 0 && (saved == ENOMEM ||
      (transmit && (saved == EAGAIN || saved == EWOULDBLOCK))))
    sample_failed_io();
  if (value < 0 && transmit && (saved == EAGAIN || saved == EWOULDBLOCK))
    __atomic_fetch_add(&g_io[6], 1, __ATOMIC_RELAXED);
  if (value >= 0)
    {
      __atomic_fetch_add(&g_io[transmit ? 0 : 1], (unsigned int)value, __ATOMIC_RELAXED);
      __atomic_fetch_add(&g_io[transmit ? 2 : 3], 1, __ATOMIC_RELAXED);
    }
  else if (saved != EAGAIN && saved != EWOULDBLOCK && saved != EINTR && saved != EINPROGRESS)
    {
      unsigned int count = __atomic_fetch_add(&g_io[saved == ENOMEM ? 4 : 5], 1, __ATOMIC_RELAXED);
      if (count < 3)
        {
          int type = 0, loop = -1;
          socklen_t size = sizeof(type);
          struct sockaddr_storage peer;
          (void)getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size);
          size = sizeof(peer);
          /* sendto can use an unconnected socket: its explicit destination is
           * authoritative, whereas getpeername would only report ENOTCONN.
           * Record a class, never the address, port or media payload. */
          if (destination == NULL &&
              getpeername(fd, (struct sockaddr *)&peer, &size) == 0)
            destination = &peer;
          if (destination != NULL && destination->ss_family == AF_INET)
            loop = (ntohl(((const struct sockaddr_in *)destination)->sin_addr.s_addr) >> 24) == 127;
#if defined(CONFIG_NET_IPv6) || defined(VISION_AGORA_HOST_TEST)
          else if (destination != NULL && destination->ss_family == AF_INET6)
            loop = IN6_IS_ADDR_LOOPBACK(&((const struct sockaddr_in6 *)destination)->sin6_addr);
#endif
          __atomic_store_n(&g_last_errno, saved, __ATOMIC_RELAXED);
          __atomic_store_n(&g_last_type, type, __ATOMIC_RELAXED);
          __atomic_store_n(&g_last_loop, loop, __ATOMIC_RELAXED);
          syslog(LOG_ERR, "AGORA NET io tx=%d bytes=%lu errno=%d type=%d peer_loop=%d\n",
                 transmit, (unsigned long)requested, saved, type, loop);
        }
    }
  errno = saved;
}
static int result(const char *operation, ssize_t n)
{
  int error = errno;
  if (n < 0 && error != EAGAIN && error != EWOULDBLOCK &&
      error != EINPROGRESS && error != EINTR &&
      __atomic_fetch_add(&g_net_errors, 1, __ATOMIC_RELAXED) < 12)
    syslog(LOG_ERR, "AGORA NET operation=%s errno=%d\n", operation, error);
  errno = error;
  return n < 0 ? aosl_hal_errno_convert(error) : (int)n;
}
static int to_os(const aosl_sockaddr_t *a, struct sockaddr_storage *s)
{
  memset(s, 0, sizeof(*s));
  if (!a) { errno = EINVAL; return -1; }
  if (a->sa_family == AOSL_AF_INET)
    {
      struct sockaddr_in *v = (struct sockaddr_in *)s;
      v->sin_family = AF_INET; v->sin_port = a->sa_port; v->sin_addr.s_addr = a->sin_addr;
      return sizeof(*v);
    }
#if defined(CONFIG_NET_IPv6) || defined(VISION_AGORA_HOST_TEST)
  if (a->sa_family == AOSL_AF_INET6)
    {
      struct sockaddr_in6 *v = (struct sockaddr_in6 *)s;
      v->sin6_family = AF_INET6; v->sin6_port = a->sa_port;
      v->sin6_flowinfo = a->sin6_flowinfo; v->sin6_scope_id = a->sin6_scope_id;
      memcpy(&v->sin6_addr, a->sin6_addr, 16); return sizeof(*v);
    }
#endif
  errno = EAFNOSUPPORT; return -1;
}
static int from_os(const struct sockaddr *s, socklen_t size, aosl_sockaddr_t *a)
{
  if (!s || !a) { errno = EINVAL; return -1; }
  memset(a, 0, sizeof(*a));
  if (s->sa_family == AF_INET && size >= sizeof(struct sockaddr_in))
    {
      const struct sockaddr_in *v = (const struct sockaddr_in *)s;
      a->sa_family = AOSL_AF_INET; a->sa_port = v->sin_port; a->sin_addr = v->sin_addr.s_addr;
      return 0;
    }
#if defined(CONFIG_NET_IPv6) || defined(VISION_AGORA_HOST_TEST)
  if (s->sa_family == AF_INET6 && size >= sizeof(struct sockaddr_in6))
    {
      const struct sockaddr_in6 *v = (const struct sockaddr_in6 *)s;
      a->sa_family = AOSL_AF_INET6; a->sa_port = v->sin6_port;
      a->sin6_flowinfo = v->sin6_flowinfo; a->sin6_scope_id = v->sin6_scope_id;
      memcpy(a->sin6_addr, &v->sin6_addr, 16); return 0;
    }
#endif
  errno = EAFNOSUPPORT; return -1;
}
int aosl_hal_sk_socket(enum aosl_socket_domain d, enum aosl_socket_type t,
                       enum aosl_socket_proto p)
{
  int domain;
  if (d == AOSL_AF_INET) domain = AF_INET;
#if defined(CONFIG_NET_IPv6) || defined(VISION_AGORA_HOST_TEST)
  else if (d == AOSL_AF_INET6) domain = AF_INET6;
#endif
  else { errno = EAFNOSUPPORT; return -1; }
  /* The pinned AOSL os_socket_pipe() passes protocol 0 for its UDP
   * wakeup channel. POSIX 0 selects the default protocol for the type. */
  if ((t != AOSL_SOCK_STREAM && t != AOSL_SOCK_DGRAM) ||
      (p != 0 && p != AOSL_IPPROTO_TCP && p != AOSL_IPPROTO_UDP) ||
      (p == AOSL_IPPROTO_TCP && t != AOSL_SOCK_STREAM) ||
      (p == AOSL_IPPROTO_UDP && t != AOSL_SOCK_DGRAM))
    { errno = EINVAL; return -1; }
  int fd = socket(domain, t == AOSL_SOCK_STREAM ? SOCK_STREAM : SOCK_DGRAM,
                  p == 0 ? 0 : p == AOSL_IPPROTO_TCP ? IPPROTO_TCP : IPPROTO_UDP);
  if (fd < 0) (void)result("socket", fd);
  return fd;
}
int aosl_hal_sk_bind(int fd, const aosl_sockaddr_t *a)
{
  struct sockaddr_storage s; int n = to_os(a, &s);
  return n < 0 ? -1 : result("bind", bind(fd, (struct sockaddr *)&s, n));
}
int aosl_hal_sk_connect(int fd, const aosl_sockaddr_t *a)
{
  struct sockaddr_storage s; int n = to_os(a, &s);
  if (n < 0) return -1;
  int ret = connect(fd, (struct sockaddr *)&s, n);
  if (ret == 0 || errno != EINPROGRESS) return result("connect", ret);
  /* The pinned AOSL core reports READY immediately after a zero return and
   * has no pending-connect/SO_ERROR state. Wait here rather than pretending
   * EINPROGRESS is success. This bounded wait can occupy the MPQ worker for
   * at most five seconds; an asynchronous core port is a separate change.
   */
  if (fd < 0 || fd >= FD_SETSIZE) { errno = EINVAL; return result("connect-fd", -1); }
  struct timespec clock;
  if (clock_gettime(CLOCK_MONOTONIC, &clock) < 0)
    return result("connect-clock", -1);
  uint64_t deadline = (uint64_t)clock.tv_sec * 1000000000u + clock.tv_nsec +
                      (uint64_t)AGORA_CONNECT_MS * 1000000u;
  do
    {
      if (clock_gettime(CLOCK_MONOTONIC, &clock) < 0)
        return result("connect-clock", -1);
      uint64_t now = (uint64_t)clock.tv_sec * 1000000000u + clock.tv_nsec;
      if (now >= deadline) { errno = ETIMEDOUT; return result("connect-wait", -1); }
      uint64_t remaining_us = (deadline - now + 999u) / 1000u;
      struct timeval timeout = { .tv_sec = remaining_us / 1000000u,
                                .tv_usec = remaining_us % 1000000u };
      /* SDK iofd_connect treats a negative HAL result as connect failure.
       * A signal is not that failure. Rebuild select's modified sets and
       * retain the original deadline, rather than restarting five seconds. */
      fd_set ready;
      FD_ZERO(&ready); FD_SET(fd, &ready);
      ret = select(fd + 1, NULL, &ready, NULL, &timeout);
    }
  while (ret < 0 && errno == EINTR);
  if (ret <= 0)
    {
      if (ret == 0) errno = ETIMEDOUT;
      return result("connect-wait", -1);
    }
  int error = 0;
  socklen_t size = sizeof(error);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0)
    return result("connect-error", -1);
  if (error) { errno = error; return result("connect-error", -1); }
  return 0;
}
int aosl_hal_sk_listen(int fd, int backlog) { return result("listen", listen(fd, backlog)); }
int aosl_hal_sk_accept(int fd, aosl_sockaddr_t *a)
{
  struct sockaddr_storage s; socklen_t n = sizeof(s);
  int client = accept(fd, (struct sockaddr *)&s, &n);
  if (client < 0) return result("accept", client);
  if (a && from_os((struct sockaddr *)&s, n, a) < 0) { close(client); return -1; }
  return client;
}
int aosl_hal_sk_close(int fd) { return result("close", close(fd)); }
int aosl_hal_sk_send(int fd, const void *p, size_t n, int flags)
{ if (n > INT_MAX) return -1; ssize_t v = send(fd, p, n, flags); io_diagnostic(fd, n, v, true, NULL); return result("send", v); }
int aosl_hal_sk_recv(int fd, void *p, size_t n, int flags)
{ if (n > INT_MAX) return -1; ssize_t v = recv(fd, p, n, flags); io_diagnostic(fd, n, v, false, NULL); return result("recv", v); }
int aosl_hal_sk_read(int fd, void *p, size_t n)
{ if (n > INT_MAX) return -1; ssize_t v = read(fd, p, n); io_diagnostic(fd, n, v, false, NULL); return result("read", v); }
int aosl_hal_sk_write(int fd, const void *p, size_t n)
{ if (n > INT_MAX) return -1; ssize_t v = write(fd, p, n); io_diagnostic(fd, n, v, true, NULL); return result("write", v); }
int aosl_hal_sk_sendto(int fd, const void *p, size_t n, int flags, const aosl_sockaddr_t *a)
{
  struct sockaddr_storage s; int size = to_os(a, &s);
  if (size < 0 || n > INT_MAX) return -1;
  ssize_t sent = sendto(fd, p, n, flags, (struct sockaddr *)&s, size);
  io_diagnostic(fd, n, sent, true, &s);
  int error = errno;
  if (sent < 0 && error == EMSGSIZE &&
      __atomic_fetch_add(&g_datagram_reports, 1, __ATOMIC_RELAXED) < 3)
    {
      /* Length and address family only; never expose RTC payload/token. */
      syslog(LOG_ERR, "AGORA NET oversized datagram bytes=%lu family=%d\n",
             (unsigned long)n, s.ss_family);
    }
  errno = error;
  return result("sendto", sent);
}
int aosl_hal_sk_recvfrom(int fd, void *p, size_t n, int flags, aosl_sockaddr_t *a)
{
  struct sockaddr_storage s; socklen_t size = sizeof(s);
  ssize_t got;
  if (n > INT_MAX) return -1;
  got = recvfrom(fd, p, n, flags, (struct sockaddr *)&s, &size);
  io_diagnostic(fd, n, got, false, NULL);
  if (got >= 0 && a && from_os((struct sockaddr *)&s, size, a) < 0) return -1;
  return result("recvfrom", got);
}
int aosl_hal_sk_set_nonblock(int fd)
{
  int flags = fcntl(fd, F_GETFL);
  return flags < 0 ? result("getflags", flags) :
         result("nonblock", fcntl(fd, F_SETFL, flags | O_NONBLOCK));
}
int aosl_hal_sk_get_sockname(int fd, aosl_sockaddr_t *a)
{
  struct sockaddr_storage s; socklen_t n = sizeof(s);
  if (getsockname(fd, (struct sockaddr *)&s, &n) < 0) return result("sockname", -1);
  return from_os((struct sockaddr *)&s, n, a);
}
int aosl_hal_sk_get_local_ip(aosl_sockaddr_t *a)
{
  struct ifreq req;
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  int ret;
  if (fd < 0) return result("local-ip-socket", -1);
  memset(&req, 0, sizeof(req)); strncpy(req.ifr_name, "wlan0", sizeof(req.ifr_name) - 1);
  ret = ioctl(fd, SIOCGIFADDR, &req);
  if (ret == 0) ret = from_os(&req.ifr_addr, sizeof(req.ifr_addr), a);
  else ret = result("local-ip", -1);
  close(fd); return ret;
}
int aosl_hal_gethostbyname(const char *host, aosl_sockaddr_t *a, int capacity)
{
  struct addrinfo hints = {0}, *list = NULL, *p;
  int count = 0;
  if (!host || !a || capacity <= 0) return 0;
  hints.ai_family = AF_UNSPEC;
  /* Use the resolver's correctly sized address directly, never copy IPv6
   * into struct sockaddr (which is smaller than sockaddr_in6).
   */
  int ret = getaddrinfo(host, NULL, &hints, &list);
  if (ret)
    {
      if (__atomic_fetch_add(&g_dns_reports, 1, __ATOMIC_RELAXED) < 8)
        syslog(LOG_ERR, "AGORA NET dns ret=%d count=0\n", ret);
      return 0;
    }
  for (p = list; p && count < capacity; p = p->ai_next)
    if (from_os(p->ai_addr, p->ai_addrlen, &a[count]) == 0) count++;
  freeaddrinfo(list);
  if (__atomic_fetch_add(&g_dns_reports, 1, __ATOMIC_RELAXED) < 8)
    syslog(LOG_INFO, "AGORA NET dns ret=0 count=%d\n", count);
  return count;
}

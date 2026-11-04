/* SPDX-License-Identifier: Apache-2.0 */
/* Execute the real network-preparation function; radio/storage are mocked. */
#ifdef VISION_BADGE_PROVISION_TEST
#define __NuttX__ 1
#define CONFIG_BK7258_AP_CORE 1
#define CONFIG_CONTEST2026_441_AGORA 1
#define CONFIG_CONTEST2026_441_VISION_BADGE_MIMO_API_KEY_PATH "/mnt/sdnand/prov/key.txt"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *test_directory;
static unsigned int prepares, unmounts;
static int storage_error;
static int agora_control_prepare_storage(void)
{ prepares++; return storage_error; }
static int test_umount(const char *path)
{ assert(!strcmp(path, "/mnt/sdnand")); unmounts++; return 0; }
static const char *local_path(const char *path, char *buffer, size_t size)
{
  const char *prefix = "/mnt/sdnand/prov";
  assert(!strncmp(path, prefix, strlen(prefix)));
  assert(snprintf(buffer, size, "%s%s", test_directory,
                  path + strlen(prefix)) < (int)size);
  return buffer;
}
static FILE *test_fopen(const char *path, const char *mode)
{ char buffer[1024]; return fopen(local_path(path, buffer, sizeof(buffer)), mode); }
static int test_mkdir(const char *path, mode_t mode)
{ char buffer[1024]; return mkdir(local_path(path, buffer, sizeof(buffer)), mode); }
static int test_unlink(const char *path)
{ char buffer[1024]; return unlink(local_path(path, buffer, sizeof(buffer))); }
static int test_rename(const char *old, const char *new)
{
  char a[1024], b[1024];
  return rename(local_path(old, a, sizeof(a)), local_path(new, b, sizeof(b)));
}
#define fopen test_fopen
#define mkdir test_mkdir
#define unlink test_unlink
#define rename test_rename
#define umount test_umount
#include "mimo-provision-test.inc"

int main(int argc, char **argv)
{
  uint8_t record[MIMO_PROVISION_RECORD_SIZE];
  char ssid[33], password[64], key[513];
  FILE *file = NULL;
  bool mounted = true;
  assert(argc == 2);
  test_directory = argv[1];
  /* Already mounted board volume: reuse storage helper, never unmount it. */
  assert(mimo_http_save_provision("test", "testpass", "test-key") == 0);
  assert(prepares == 1 && unmounts == 0);
  assert(mimo_open_provision_record(&file, &mounted) == 0 && !mounted);
  assert(fread(record, 1, sizeof(record), file) == sizeof(record));
  fclose(file);
  assert(mimo_decode_network_credentials(record, sizeof(record), ssid,
         sizeof(ssid), password, sizeof(password)) == 0);
  assert(!strcmp(ssid, "test") && !strcmp(password, "testpass"));
  assert(mimo_decode_provision_record(record, sizeof(record), key, sizeof(key)) == 0);
  assert(!strcmp(key, "test-key"));
  assert(mimo_open_credentials(&file, &mounted) == 0 && !mounted);
  fclose(file);
  assert(prepares == 1);
  /* A second save replaces the persisted record and remains readable. */
  assert(mimo_http_save_provision("changed", "newpass", "new-key") == 0);
  storage_error = -ENODEV;
  assert(mimo_http_save_provision("lost", "lostpass", "lost-key") == -ENODEV);
  assert(mimo_open_provision_record(&file, &mounted) == 0 && !mounted);
  assert(fread(record, 1, sizeof(record), file) == sizeof(record)); fclose(file);
  assert(mimo_decode_network_credentials(record, sizeof(record), ssid,
         sizeof(ssid), password, sizeof(password)) == 0 && !strcmp(ssid, "changed"));
  assert(mimo_decode_provision_record(record, sizeof(record), key, sizeof(key)) == 0);
  assert(!strcmp(key, "new-key"));
  record[14] ^= 1;
  assert(mimo_decode_network_credentials(record, sizeof(record), ssid,
         sizeof(ssid), password, sizeof(password)) == -EBADMSG);
  assert(unlink("/mnt/sdnand/prov/wifi.bin") == 0);
  assert(mimo_open_provision_record(&file, &mounted) == -ENODEV && !mounted);
  assert(mimo_open_credentials(&file, &mounted) == -ENODEV && !mounted);
  storage_error = 0;
  assert(mimo_open_provision_record(&file, &mounted) == -ENOENT && !mounted);
  assert(mimo_open_credentials(&file, &mounted) == -ENOENT && !mounted);
  assert(unmounts == 0);
  assert(mimo_http_save_provision(NULL, "x", "x") == -EINVAL);
  puts("PASS: real provision save/reload/replace/CRC/error paths; shared mount retained; storage mocked");
  return 0;
}
#else
#define __NuttX__ 1
#define CONFIG_BK7258_AP_CORE 1
#define CONFIG_BK7258_WIFI_VNET 1
#include "../../chips/bk7258/include/bk7258_wifi.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#define MIMO_PROVISION_RECORD_SIZE 818
#define MIMO_PROVISION_PSK_SIZE 63
#define MIMO_PROVISION_SSID_SIZE 32
#define MSEC2TICK(ms) (ms)
static struct bk7258_wifi_result_s link;
static int read_error, lease_error, provision_error = -ENOENT, async_error;
static unsigned int reads, sets, opens, connects, polls, cancels;
static bool native;
static clock_t ticks;
static clock_t clock_systime_ticks(void) { return ticks; }
static void nxsig_usleep(unsigned int usec) { ticks += usec / 1000; }
static int umount(const char *path) { (void)path; return 0; }
int bk7258_wifi_read_link(struct bk7258_wifi_result_s *out)
{ reads++; *out = link; return read_error; }
bool bk7258_wifi_native_lease_matches(const struct bk7258_wifi_result_s *state)
{ assert(state->ipaddr == link.ipaddr); return native; }
int bk7258_wifi_set_native_lease(const struct bk7258_wifi_result_s *state)
{
  assert(state->link_state == BK7258_WIFI_LINK_CONNECTED && state->ipaddr);
  sets++; if (!lease_error) native = true; return lease_error;
}
static int mimo_open_provision_record(FILE **file, bool *mounted)
{
  unsigned char record[MIMO_PROVISION_RECORD_SIZE] = {0};
  opens++;
  if (provision_error) return provision_error;
  *file = tmpfile(); assert(*file);
  assert(fwrite(record, 1, sizeof(record), *file) == sizeof(record));
  rewind(*file); *mounted = false; return 0;
}
static int mimo_decode_network_credentials(const uint8_t *record, size_t size,
                                           char *ssid, size_t ssid_size,
                                           char *password, size_t password_size)
{
  (void)record; assert(size == MIMO_PROVISION_RECORD_SIZE);
  assert(ssid_size > 4 && password_size > 8);
  strcpy(ssid, "test"); strcpy(password, "testpass"); return 0;
}
int bk7258_wifi_connect_async(const char *ssid, const char *password,
                             uint32_t timeout, uint32_t *ticket)
{
  assert(!strcmp(ssid, "test") && !strcmp(password, "testpass"));
  assert(timeout == BK7258_WIFI_CONNECT_DEFAULT_MS);
  connects++; *ticket = 7; return async_error;
}
int bk7258_wifi_connect_poll(uint32_t ticket, struct bk7258_wifi_result_s *out)
{ assert(ticket == 7); polls++; *out = link; return 0; }
int bk7258_wifi_connect_cancel(uint32_t ticket)
{ assert(ticket == 7); cancels++; return 0; }
#include "mimo-network-test.inc"
int main(void)
{
  link.link_state = BK7258_WIFI_LINK_CONNECTED;
  link.ipaddr = 1; link.netmask = 2; link.router = 3;
  native = true;
  assert(mimo_http_prepare_network() == 0);
  assert(mimo_http_prepare_network() == 0 && reads == 2 && sets == 0);
  assert(opens == 0 && connects == 0);
  native = false;
  assert(mimo_http_prepare_network() == 0 && sets == 1 && native);
  native = false; lease_error = -EIO;
  assert(mimo_http_prepare_network() == -EIO && sets == 2);
  assert(opens == 0 && connects == 0);
  lease_error = 0; read_error = -EAGAIN;
  assert(mimo_http_prepare_network() == -EAGAIN && opens == 0);
  read_error = 0;
  link.link_state = BK7258_WIFI_LINK_DISCONNECTED; link.ipaddr = 0;
  assert(mimo_http_prepare_network() == -ENETDOWN && opens == 1);
  assert(connects == 0); /* No stale success after earlier good requests. */
  provision_error = 0; async_error = -EBUSY;
  assert(mimo_http_prepare_network() == -EBUSY && connects == 1 && polls == 0);
  async_error = 0; link.status = -ETIMEDOUT;
  assert(mimo_http_prepare_network() == -ETIMEDOUT && polls == 1);
  assert(sets == 2 && cancels == 0);
  link.status = 0; link.link_state = BK7258_WIFI_LINK_CONNECTED;
  link.ipaddr = 4; native = false;
  assert(mimo_http_prepare_network() == 0 && sets == 3 && native);
  assert(connects == 2); /* Adopt an existing changed lease; no extra scan. */
  puts("PASS: fresh CP link/native lease checks, disconnect, repair/error paths; hardware mocked");
}
#endif

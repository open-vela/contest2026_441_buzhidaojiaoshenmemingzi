/* SPDX-License-Identifier: Apache-2.0 */

/* ========================================================================
 * mimo_http.c —— MiMo HTTPS 通信基础设施
 *
 * 这个模块是整个项目网络层的"地基"，被摄像头链路和语音链路共同使用。
 * 它封装了以下功能：
 *   1. TLS 安全连接（mbedTLS）
 *   2. 时钟同步（NTP 或 HTTPS 校时后备）
 *   3. 凭据加载（从 SD-NAND 读取 API Key）
 *   4. HTTP POST 请求（支持流式请求体和流式响应）
 *   5. JSON 响应解析
 *
 * 为什么不直接用 webclient？
 *   因为 webclient 不支持 mbedTLS 的证书链验证和域名校验。
 *   本项目需要严格的安全校验，所以自己封装了 TLS 层。
 * ======================================================================== */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <syslog.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vision_badge/config.h>
#include <vision_badge/http_date.h>
#include <vision_badge/mimo_http.h>

#ifdef CONFIG_CRYPTO_MBEDTLS
#ifdef __NuttX__
#include <nuttx/mutex.h>
#endif
#include <nuttx/signal.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <netutils/cJSON.h>
#include <netutils/webclient.h>
#ifdef CONFIG_NETUTILS_NTPCLIENT
#include <netutils/ntpclient.h>
#endif
#ifdef CONFIG_BK7258_WIFI_VNET
#include <arch/chip/bk7258_wifi.h>
#endif
#ifdef __NuttX__
#include <sys/mount.h>
#endif

/* ---- 服务器地址配置 ---- */
#define MIMO_TLS_API_HOST  "api.xiaomimimo.com"           /* 普通 API Key */
#define MIMO_TLS_PLAN_HOST "token-plan-cn.xiaomimimo.com" /* Token Plan Key */
#define MIMO_TLS_PORT      "443"
#define MIMO_TLS_PATH      "/v1/chat/completions"
#define MIMO_CLOCK_MIN_YEAR 2024
#define MIMO_CLOCK_WAIT_SECONDS 12
#define MIMO_HTTP_HEADER_SIZE 1536
#define MIMO_PROVISION_RECORD_SIZE 818
#define MIMO_PROVISION_API_OFFSET 109
#define MIMO_PROVISION_API_SIZE 512
#define MIMO_PROVISION_SSID_LEN_OFFSET 12
#define MIMO_PROVISION_PSK_LEN_OFFSET 13
#define MIMO_PROVISION_SSID_OFFSET 14
#define MIMO_PROVISION_PSK_OFFSET 46
#define MIMO_PROVISION_SSID_SIZE 32
#define MIMO_PROVISION_PSK_SIZE 63
#define MIMO_PROVISION_RESERVED_OFFSET 813
#define MIMO_PROVISION_CRC_OFFSET 814

/* The CP/AP Wi-Fi service has one connection per boot.  Keep the successful
 * result in the application so a second vision request does not re-run the
 * scan and WPA handshake. */
static bool g_mimo_network_ready;

#ifdef __NuttX__
static mutex_t g_mimo_session_lock = NXMUTEX_INITIALIZER;
#endif

static uint32_t mimo_provision_crc32(const uint8_t *data, size_t size);

#ifdef __NuttX__
int mimo_http_save_provision(const char *ssid, const char *password,
                             const char *api_key)
{
  const char *device = CONFIG_CONTEST2026_441_VISION_BADGE_MIMO_STORAGE_DEVICE;
  uint8_t record[MIMO_PROVISION_RECORD_SIZE];
  const char *temporary = "/mnt/sdnand/prov/wifi.tmp";
  const char *final = "/mnt/sdnand/prov/wifi.bin";
  size_t ssid_length;
  size_t password_length;
  size_t key_length;
  uint32_t crc;
  FILE *file = NULL;
  bool mounted = false;
  int ret = 0;

  if (ssid == NULL || password == NULL || api_key == NULL)
    {
      return -EINVAL;
    }

  ssid_length = strlen(ssid);
  password_length = strlen(password);
  key_length = strlen(api_key);
  if (ssid_length == 0 || ssid_length > MIMO_PROVISION_SSID_SIZE ||
      password_length > MIMO_PROVISION_PSK_SIZE || key_length == 0 ||
      key_length >= MIMO_PROVISION_API_SIZE)
    {
      return -EINVAL;
    }

  memset(record, 0, sizeof(record));
  memcpy(record, "VSWP", 4);
  record[4] = 3;
  record[MIMO_PROVISION_SSID_LEN_OFFSET] = (uint8_t)ssid_length;
  record[MIMO_PROVISION_PSK_LEN_OFFSET] = (uint8_t)password_length;
  memcpy(record + MIMO_PROVISION_SSID_OFFSET, ssid, ssid_length);
  memcpy(record + MIMO_PROVISION_PSK_OFFSET, password, password_length);
  memcpy(record + MIMO_PROVISION_API_OFFSET, api_key, key_length);
  crc = mimo_provision_crc32(record, MIMO_PROVISION_CRC_OFFSET);
  record[MIMO_PROVISION_CRC_OFFSET] = (uint8_t)crc;
  record[MIMO_PROVISION_CRC_OFFSET + 1] = (uint8_t)(crc >> 8);
  record[MIMO_PROVISION_CRC_OFFSET + 2] = (uint8_t)(crc >> 16);
  record[MIMO_PROVISION_CRC_OFFSET + 3] = (uint8_t)(crc >> 24);

  (void)mkdir("/mnt/sdnand", 0777);
  if (stat("/dev/mmcsd0p0", &(struct stat){0}) == 0)
    {
      device = "/dev/mmcsd0p0";
    }

  if (mount(device, "/mnt/sdnand", "vfat", 0, NULL) == 0)
    {
      mounted = true;
    }
  else if (errno != EBUSY)
    {
      ret = -errno;
      goto out;
    }

  if (mkdir("/mnt/sdnand/prov", 0777) < 0 && errno != EEXIST)
    {
      ret = -errno;
      goto out;
    }

  file = fopen(temporary, "wb");
  if (file == NULL)
    {
      ret = -errno;
      goto out;
    }

  if (fwrite(record, 1, sizeof(record), file) != sizeof(record) ||
      fflush(file) != 0 || fsync(fileno(file)) != 0)
    {
      ret = errno != 0 ? -errno : -EIO;
      goto out;
    }

  if (fclose(file) != 0)
    {
      file = NULL;
      ret = -errno;
      goto out;
    }
  file = NULL;

  if (unlink(final) != 0 && errno != ENOENT)
    {
      ret = -errno;
      goto out;
    }

  if (rename(temporary, final) != 0)
    {
      ret = -errno;
      goto out;
    }

out:
  if (file != NULL)
    {
      (void)fclose(file);
    }

  if (ret < 0)
    {
      (void)unlink(temporary);
    }

  if (mounted)
    {
      (void)umount("/mnt/sdnand");
    }

  memset(record, 0, sizeof(record));
  return ret;
}
#endif

/* DigiCert Global Root G2（公开信任锚，非密钥） */
static const char g_digicert_global_root_g2[] =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
  "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
  "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
  "MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n"
  "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
  "b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
  "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
  "2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
  "1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
  "q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
  "tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
  "vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\n"
  "BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\n"
  "5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\n"
  "1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\n"
  "NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\n"
  "Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\n"
  "8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\n"
  "pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\n"
  "MrY=\n"
  "-----END CERTIFICATE-----\n";

static FILE *mimo_open_credential_file(void)
{
  static const char *paths[] =
  {
    "/mnt/sdnand/prov/wifi.bin",
    "/mnt/sdnand/prov/vela.cfg",
    CONFIG_CONTEST2026_441_VISION_BADGE_MIMO_API_KEY_PATH
  };
  FILE *file;
  size_t i;

  for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
    {
      file = fopen(paths[i], "rb");
      if (file != NULL)
        {
          return file;
        }
    }

  return NULL;
}

static uint32_t mimo_provision_u32(const uint8_t *data)
{
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint32_t mimo_provision_crc32(const uint8_t *data, size_t size)
{
  uint32_t crc = 0xffffffffu;
  size_t i;

  for (i = 0; i < size; i++)
    {
      unsigned int bit;

      crc ^= data[i];
      for (bit = 0; bit < 8; bit++)
        {
          crc = (crc >> 1) ^
                (0xedb88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }

  return crc ^ 0xffffffffu;
}

static int mimo_decode_provision_record(const uint8_t *record, size_t size,
                                        char *api_key,
                                        size_t api_key_size)
{
  const uint8_t *stored_key = record + MIMO_PROVISION_API_OFFSET;
  size_t key_size = 0;
  size_t i;

  if (size != MIMO_PROVISION_RECORD_SIZE ||
      memcmp(record, "VSWP", 4) != 0 ||
      record[4] != 3 || record[5] != 0 ||
      record[MIMO_PROVISION_RESERVED_OFFSET] != 0 ||
      mimo_provision_u32(record + MIMO_PROVISION_CRC_OFFSET) !=
      mimo_provision_crc32(record, MIMO_PROVISION_CRC_OFFSET))
    {
      return -EBADMSG;
    }

  while (key_size < MIMO_PROVISION_API_SIZE && stored_key[key_size] != 0)
    {
      key_size++;
    }

  if (key_size == 0)
    {
      return -ENOENT;
    }

  if (key_size >= api_key_size)
    {
      return -E2BIG;
    }

  for (i = key_size; i < MIMO_PROVISION_API_SIZE; i++)
    {
      if (stored_key[i] != 0)
        {
          return -EBADMSG;
        }
    }

  memcpy(api_key, stored_key, key_size);
  api_key[key_size] = '\0';
  return 0;
}

static int mimo_decode_network_credentials(const uint8_t *record, size_t size,
                                           char *ssid, size_t ssid_size,
                                           char *password,
                                           size_t password_size)
{
  char api_key[MIMO_PROVISION_API_SIZE + 1];
  size_t stored_ssid;
  size_t stored_password;
  int ret;

  ret = mimo_decode_provision_record(record, size, api_key, sizeof(api_key));
  memset(api_key, 0, sizeof(api_key));
  if (ret < 0)
    {
      return ret;
    }

  stored_ssid = record[MIMO_PROVISION_SSID_LEN_OFFSET];
  stored_password = record[MIMO_PROVISION_PSK_LEN_OFFSET];
  if (stored_ssid == 0 || stored_ssid > MIMO_PROVISION_SSID_SIZE ||
      stored_password > MIMO_PROVISION_PSK_SIZE ||
      stored_ssid >= ssid_size || stored_password >= password_size)
    {
      return -EBADMSG;
    }

  memcpy(ssid, record + MIMO_PROVISION_SSID_OFFSET, stored_ssid);
  ssid[stored_ssid] = '\0';
  memcpy(password, record + MIMO_PROVISION_PSK_OFFSET, stored_password);
  password[stored_password] = '\0';
  return 0;
}

#ifdef __NuttX__
static int mimo_open_credentials(FILE **file, bool *mounted)
{
  const char *device = CONFIG_CONTEST2026_441_VISION_BADGE_MIMO_STORAGE_DEVICE;
  int ret;

  *mounted = false;
  *file = mimo_open_credential_file();
  if (*file != NULL)
    {
      return 0;
    }

  (void)mkdir("/mnt/sdnand", 0777);
  if (stat("/dev/mmcsd0p0", &(struct stat){0}) == 0)
    {
      device = "/dev/mmcsd0p0";
    }

  ret = mount(device, "/mnt/sdnand", "vfat", 0, NULL);
  if (ret < 0 && errno != EBUSY)
    {
      return -errno;
    }

  *mounted = ret == 0;
  *file = mimo_open_credential_file();
  if (*file == NULL)
    {
      ret = -errno;
      if (*mounted)
        {
          (void)umount("/mnt/sdnand");
        }

      return ret;
    }

  return 0;
}

static int mimo_open_provision_record(FILE **file, bool *mounted)
{
  const char *device = CONFIG_CONTEST2026_441_VISION_BADGE_MIMO_STORAGE_DEVICE;
  int ret;

  *mounted = false;
  *file = fopen("/mnt/sdnand/prov/wifi.bin", "rb");
  if (*file == NULL)
    {
      *file = fopen("/mnt/sdnand/prov/vela.cfg", "rb");
    }

  if (*file != NULL)
    {
      return 0;
    }

  (void)mkdir("/mnt/sdnand", 0777);
  if (stat("/dev/mmcsd0p0", &(struct stat){0}) == 0)
    {
      device = "/dev/mmcsd0p0";
    }

  ret = mount(device, "/mnt/sdnand", "vfat", 0, NULL);
  if (ret < 0 && errno != EBUSY)
    {
      return -errno;
    }

  *mounted = ret == 0;
  *file = fopen("/mnt/sdnand/prov/wifi.bin", "rb");
  if (*file == NULL)
    {
      *file = fopen("/mnt/sdnand/prov/vela.cfg", "rb");
    }

  if (*file == NULL)
    {
      ret = -errno;
      if (*mounted)
        {
          (void)umount("/mnt/sdnand");
        }

      return ret;
    }

  return 0;
}
#else
static int mimo_open_credentials(FILE **file, bool *mounted)
{
  *mounted = false;
  *file = mimo_open_credential_file();
  return *file == NULL ? -errno : 0;
}
#endif

struct mimo_tls_context_s
{
  mbedtls_net_context net;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_x509_crt ca;
  mbedtls_ssl_config config;
  mbedtls_ssl_context ssl;
  uint32_t ignored_time_flags;
  uint32_t remaining_verify_flags;
};

static int mimo_https_bootstrap_clock(void);

static bool mimo_clock_ready(struct tm *utc)
{
  time_t now = time(NULL);
  struct tm value;

  if (gmtime_r(&now, &value) == NULL ||
      value.tm_year + 1900 < MIMO_CLOCK_MIN_YEAR)
    {
      return false;
    }

  if (utc != NULL)
    {
      *utc = value;
    }

  return true;
}

/* ========================================================================
 * mimo_http_sync_clock —— 时钟同步
 *
 * 因为 TLS 证书验证需要系统时间在有效期内，而 BK7258 没有 RTC 电池，
 * 每次开机时间都是 1970-01-01。所以必须先同步时钟。
 *
 * 同步策略（两步走）：
 *   1. 尝试 NTP（UDP/123），等待最多 12 秒
 *   2. 如果 NTP 失败（如校园网封锁 UDP/123），改用 HTTPS 校时后备：
 *      - 先临时忽略证书时间，建立 TLS 连接到 MiMo API
 *      - 从 HTTP 响应头的 Date 字段获取服务器时间
 *      - 用这个时间设置系统时钟
 *      - 关闭连接，后续请求使用严格证书验证
 * ======================================================================== */
int mimo_http_sync_clock(void)
{
  struct tm utc;
  int ret;

#ifdef CONFIG_NETUTILS_NTPCLIENT
  if (!mimo_clock_ready(&utc))
    {
      int pid;
      int i;

      printf("mimo_http: system time is not usable; starting NTP\n");
      pid = ntpc_start();
      if (pid < 0)
        {
          fprintf(stderr, "mimo_http: NTP start failed: %d\n", pid);
          return pid;
        }

      for (i = 0; i < MIMO_CLOCK_WAIT_SECONDS; i++)
        {
          if (mimo_clock_ready(&utc))
            {
              break;
            }

          sleep(1);
        }
    }
#endif

  if (!mimo_clock_ready(&utc))
    {
      printf("mimo_http: NTP unavailable after %d seconds; "
             "trying authenticated HTTPS time\n",
             MIMO_CLOCK_WAIT_SECONDS);
      ret = mimo_https_bootstrap_clock();
      if (ret < 0)
        {
          fprintf(stderr,
                  "mimo_http: neither NTP nor authenticated HTTPS set "
                  "the clock: %d\n", ret);
          return ret;
        }

      if (!mimo_clock_ready(&utc))
        {
          fprintf(stderr,
                  "mimo_http: HTTPS reported success but the system "
                  "clock is still unusable\n");
          return -EIO;
        }
    }

  printf("mimo_http: UTC %04d-%02d-%02d %02d:%02d:%02d\n",
         utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
         utc.tm_hour, utc.tm_min, utc.tm_sec);
  return 0;
}

/* ---- TLS 基础设施 ---- */

static void mimo_tls_free(struct mimo_tls_context_s *tls)
{
  mbedtls_ssl_close_notify(&tls->ssl);
  mbedtls_net_free(&tls->net);
  mbedtls_ssl_free(&tls->ssl);
  mbedtls_ssl_config_free(&tls->config);
  mbedtls_x509_crt_free(&tls->ca);
  mbedtls_ctr_drbg_free(&tls->ctr_drbg);
  mbedtls_entropy_free(&tls->entropy);
}

static int mimo_tls_bootstrap_verify(void *arg, mbedtls_x509_crt *certificate,
                                     int depth, uint32_t *flags)
{
  struct mimo_tls_context_s *tls = arg;
  const uint32_t time_flags = MBEDTLS_X509_BADCERT_EXPIRED |
                              MBEDTLS_X509_BADCERT_FUTURE;

  (void)certificate;
  (void)depth;
  tls->ignored_time_flags |= *flags & time_flags;
  *flags &= ~time_flags;
  tls->remaining_verify_flags |= *flags;
  return 0;
}

static int mimo_tls_prepare(struct mimo_tls_context_s *tls,
                            const char *hostname,
                            unsigned int timeout_ms,
                            bool bootstrap_time)
{
  static const unsigned char personalization[] = "mimo-http-tls";
  int ret;

  mbedtls_net_init(&tls->net);
  mbedtls_entropy_init(&tls->entropy);
  mbedtls_ctr_drbg_init(&tls->ctr_drbg);
  mbedtls_x509_crt_init(&tls->ca);
  mbedtls_ssl_config_init(&tls->config);
  mbedtls_ssl_init(&tls->ssl);

  ret = mbedtls_ctr_drbg_seed(&tls->ctr_drbg, mbedtls_entropy_func,
                              &tls->entropy, personalization,
                              sizeof(personalization) - 1);
  if (ret != 0)
    {
      return ret;
    }

  ret = mbedtls_x509_crt_parse(
    &tls->ca, (const unsigned char *)g_digicert_global_root_g2,
    sizeof(g_digicert_global_root_g2));
  if (ret != 0)
    {
      return ret;
    }

  ret = mbedtls_ssl_config_defaults(&tls->config, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0)
    {
      return ret;
    }

  mbedtls_ssl_conf_authmode(&tls->config, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&tls->config, &tls->ca, NULL);
  mbedtls_ssl_conf_rng(&tls->config, mbedtls_ctr_drbg_random,
                       &tls->ctr_drbg);
  mbedtls_ssl_conf_read_timeout(&tls->config, timeout_ms);
  if (bootstrap_time)
    {
      mbedtls_ssl_conf_verify(&tls->config, mimo_tls_bootstrap_verify, tls);
    }

  ret = mbedtls_ssl_setup(&tls->ssl, &tls->config);
  if (ret != 0)
    {
      return ret;
    }

  return mbedtls_ssl_set_hostname(&tls->ssl, hostname);
}

static int mimo_tls_handshake(struct mimo_tls_context_s *tls,
                              const char *hostname, const char *port)
{
  uint32_t flags;
  int ret;

  ret = mbedtls_net_connect(&tls->net, hostname, port,
                            MBEDTLS_NET_PROTO_TCP);
  if (ret != 0)
    {
      return ret;
    }

  mbedtls_ssl_set_bio(&tls->ssl, &tls->net, mbedtls_net_send,
                      mbedtls_net_recv, mbedtls_net_recv_timeout);

  do
    {
      ret = mbedtls_ssl_handshake(&tls->ssl);
    }
  while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
         ret == MBEDTLS_ERR_SSL_WANT_WRITE);

  if (ret != 0)
    {
      return ret;
    }

  flags = mbedtls_ssl_get_verify_result(&tls->ssl);
  if (flags != 0)
    {
      fprintf(stderr, "mimo_http: certificate verify flags=0x%08lx\n",
              (unsigned long)flags);
      return -EACCES;
    }

  return 0;
}

/* ---- TLS 操作回调 ---- */

static int mimo_webclient_tls_connect(
    void *ctx, const char *hostname, const char *port,
    unsigned int timeout_sec, struct webclient_tls_connection **connection)
{
  struct mimo_tls_context_s *tls;
  char error[128];
  unsigned int timeout_ms;
  int ret;

  (void)ctx;
  tls = calloc(1, sizeof(*tls));
  if (tls == NULL)
    {
      return -ENOMEM;
    }

  timeout_ms = timeout_sec > UINT_MAX / 1000 ? UINT_MAX : timeout_sec * 1000;
  ret = mimo_tls_prepare(tls, hostname, timeout_ms, false);
  if (ret == 0)
    {
      ret = mimo_tls_handshake(tls, hostname, port);
    }

  if (ret != 0)
    {
      mbedtls_strerror(ret, error, sizeof(error));
      fprintf(stderr, "mimo_http: TLS connection failed: %d (%s)\n",
              ret, error);
      mimo_tls_free(tls);
      free(tls);
      return ret == MBEDTLS_ERR_SSL_ALLOC_FAILED ? -ENOMEM : -EIO;
    }

  *connection = (struct webclient_tls_connection *)tls;
  return 0;
}

static ssize_t mimo_webclient_tls_send(
    void *ctx, struct webclient_tls_connection *connection,
    const void *buffer, size_t size)
{
  struct mimo_tls_context_s *tls =
    (struct mimo_tls_context_s *)connection;
  int ret;

  (void)ctx;
  do
    {
      ret = mbedtls_ssl_write(&tls->ssl, buffer, size);
    }
  while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
         ret == MBEDTLS_ERR_SSL_WANT_WRITE);

  return ret < 0 ? -EIO : ret;
}

static ssize_t mimo_webclient_tls_recv(
    void *ctx, struct webclient_tls_connection *connection,
    void *buffer, size_t size)
{
  struct mimo_tls_context_s *tls =
    (struct mimo_tls_context_s *)connection;
  int ret;

  (void)ctx;
  do
    {
      ret = mbedtls_ssl_read(&tls->ssl, buffer, size);
    }
  while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
         ret == MBEDTLS_ERR_SSL_WANT_WRITE);

  if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
    {
      return 0;
    }

  return ret < 0 ? -EIO : ret;
}

static int mimo_webclient_tls_close(
    void *ctx, struct webclient_tls_connection *connection)
{
  struct mimo_tls_context_s *tls =
    (struct mimo_tls_context_s *)connection;

  (void)ctx;
  mimo_tls_free(tls);
  free(tls);
  return 0;
}

static int mimo_webclient_tls_poll(
    void *ctx, struct webclient_tls_connection *connection,
    struct webclient_poll_info *info)
{
  struct mimo_tls_context_s *tls =
    (struct mimo_tls_context_s *)connection;

  (void)ctx;
  info->fd = tls->net.fd;
  info->flags = WEBCLIENT_POLL_INFO_WANT_READ;
  return 0;
}

static const struct webclient_tls_ops g_mimo_tls_ops =
{
  .connect = mimo_webclient_tls_connect,
  .send = mimo_webclient_tls_send,
  .recv = mimo_webclient_tls_recv,
  .close = mimo_webclient_tls_close,
  .get_poll_info = mimo_webclient_tls_poll,
  .init_connection = NULL,
};

/* ---- HTTPS 时间自举 ---- */

static int mimo_tls_read_http_headers(struct mimo_tls_context_s *tls,
                                      const char *hostname, char *headers,
                                      size_t capacity)
{
  char request[256];
  size_t request_size;
  size_t written = 0;
  size_t received = 0;
  int ret;

  ret = snprintf(request, sizeof(request),
                 "GET %s HTTP/1.1\r\nHost: %s\r\n"
                 "Connection: close\r\nAccept: application/json\r\n\r\n",
                 MIMO_TLS_PATH, hostname);
  if (ret < 0 || (size_t)ret >= sizeof(request))
    {
      return -EOVERFLOW;
    }

  request_size = (size_t)ret;
  while (written < request_size)
    {
      ret = mbedtls_ssl_write(&tls->ssl,
                              (const unsigned char *)request + written,
                              request_size - written);
      if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
          ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          continue;
        }

      if (ret <= 0)
        {
          return ret != 0 ? ret : -EIO;
        }

      written += ret;
    }

  while (received < capacity - 1)
    {
      ret = mbedtls_ssl_read(&tls->ssl,
                             (unsigned char *)headers + received,
                             capacity - 1 - received);
      if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
          ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          continue;
        }

      if (ret <= 0)
        {
          return ret != 0 ? ret : -EIO;
        }

      received += (size_t)ret;
      headers[received] = '\0';
      if (strstr(headers, "\r\n\r\n") != NULL)
        {
          return strncmp(headers, "HTTP/", 5) == 0 ? 0 : -EPROTO;
        }
    }

  return -E2BIG;
}

static int mimo_https_bootstrap_clock(void)
{
  struct mimo_tls_context_s *tls;
  struct timespec value;
  struct tm utc;
  const char *stage = "tls-prepare";
  char headers[MIMO_HTTP_HEADER_SIZE];
  time_t epoch;
  int ret;

  tls = calloc(1, sizeof(*tls));
  if (tls == NULL)
    {
      return -ENOMEM;
    }

  ret = mimo_tls_prepare(tls, MIMO_TLS_API_HOST, 10000, true);
  if (ret == 0)
    {
      stage = "tls-handshake";
      ret = mimo_tls_handshake(tls, MIMO_TLS_API_HOST, MIMO_TLS_PORT);
    }

  if (ret == 0 && mimo_clock_ready(NULL))
    {
      goto out;
    }

  if (ret == 0 && (tls->ignored_time_flags == 0 ||
                   tls->remaining_verify_flags != 0))
    {
      stage = "certificate-gate";
      fprintf(stderr,
              "mimo_http: HTTPS time bootstrap did not meet the "
              "certificate gate\n");
      ret = -EACCES;
    }

  if (ret == 0)
    {
      stage = "http-headers";
      printf("mimo_http: certificate chain and hostname verified; "
             "certificate time is the only temporary exception\n");
      ret = mimo_tls_read_http_headers(tls, MIMO_TLS_API_HOST, headers,
                                       sizeof(headers));
    }

  if (ret == 0)
    {
      stage = "date-parse";
      ret = vision_http_date_parse(headers, &epoch);
    }

  if (ret == 0)
    {
      stage = "clock-set";
      value.tv_sec = epoch;
      value.tv_nsec = 0;
      if (clock_settime(CLOCK_REALTIME, &value) < 0)
        {
          ret = -errno;
        }
    }

  if (ret == 0 && !mimo_clock_ready(&utc))
    {
      stage = "clock-check";
      ret = -EIO;
    }

  if (ret == 0)
    {
      printf("mimo_http: HTTPS time bootstrap UTC "
             "%04d-%02d-%02d %02d:%02d:%02d\n",
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
             utc.tm_hour, utc.tm_min, utc.tm_sec);
    }

out:
  if (ret < 0)
    {
      syslog(LOG_ERR, "MIMO CLOCK stage=%s ret=%d\n", stage, ret);
    }

  mimo_tls_free(tls);
  free(tls);
  return ret;
}

int mimo_http_prepare_network(void)
{
#if defined(__NuttX__) && defined(CONFIG_BK7258_WIFI_VNET) && \
    defined(CONFIG_BK7258_AP_CORE)
  struct bk7258_wifi_result_s result;
  uint8_t record[MIMO_PROVISION_RECORD_SIZE];
  char password[MIMO_PROVISION_PSK_SIZE + 1];
  char ssid[MIMO_PROVISION_SSID_SIZE + 1];
  FILE *file;
  clock_t started;
  uint32_t ticket;
  size_t size;
  bool mounted;
  int ret;

  if (g_mimo_network_ready)
    {
      return 0;
    }

  memset(&result, 0, sizeof(result));
  ret = bk7258_wifi_read_link(&result);
  if (ret >= 0 && result.link_state == BK7258_WIFI_LINK_CONNECTED &&
      result.ipaddr != 0)
    {
      g_mimo_network_ready = true;
      return 0;
    }

  ret = mimo_open_provision_record(&file, &mounted);
  if (ret < 0)
    {
      return -ENETDOWN;
    }

  size = fread(record, 1, sizeof(record), file);
  if (ferror(file) != 0 || fgetc(file) != EOF)
    {
      ret = -EBADMSG;
    }
  else
    {
      ret = mimo_decode_network_credentials(record, size, ssid, sizeof(ssid),
                                            password, sizeof(password));
    }

  (void)fclose(file);
  if (mounted)
    {
      (void)umount("/mnt/sdnand");
    }

  memset(record, 0, sizeof(record));
  if (ret < 0)
    {
      memset(password, 0, sizeof(password));
      return ret;
    }

  ret = bk7258_wifi_connect_async(ssid, password,
                                  BK7258_WIFI_CONNECT_DEFAULT_MS, &ticket);
  memset(ssid, 0, sizeof(ssid));
  memset(password, 0, sizeof(password));
  if (ret < 0)
    {
      return ret;
    }

  started = clock_systime_ticks();
  for (;;)
    {
      ret = bk7258_wifi_connect_poll(ticket, &result);
      if (ret != -EAGAIN)
        {
          break;
        }

      if ((clock_t)(clock_systime_ticks() - started) >=
          MSEC2TICK(BK7258_WIFI_CONNECT_DEFAULT_MS + 2000u))
        {
          (void)bk7258_wifi_connect_cancel(ticket);
          return -ETIMEDOUT;
        }

      nxsig_usleep(100000);
    }

  if (ret < 0)
    {
      return ret;
    }

  ret = result.status < 0 ? result.status :
        (result.link_state == BK7258_WIFI_LINK_CONNECTED &&
         result.ipaddr != 0 ? 0 : -ENETDOWN);
  if (ret == 0)
    {
      g_mimo_network_ready = true;
    }

  return ret;
#else
  return -ENOSYS;
#endif
}

/* ========================================================================
 * mimo_http_load_credentials —— 从 SD-NAND 加载 API Key
 *
 * 从 /mnt/sdnand/prov/wifi.bin 读取 MiMo API Key。
 * 根据 Key 的前缀选择不同的服务器：
 *   - "tp-" 开头 → Token Plan 服务器（token-plan-cn.xiaomimimo.com）
 *   - 其他 → 普通 API 服务器（api.xiaomimimo.com）
 * ======================================================================== */
int mimo_http_load_credentials(char *api_key, size_t api_key_size,
                               const char **hostname)
{
  uint8_t stored[MIMO_PROVISION_RECORD_SIZE];
  FILE *file;
  size_t length;
  int ch;
  bool too_long;
  bool mounted;

  if (api_key == NULL || api_key_size < 2 || hostname == NULL)
    {
      return -EINVAL;
    }

  ch = mimo_open_credentials(&file, &mounted);
  if (ch < 0)
    {
      fprintf(stderr, "mimo_http: cannot open %s: %d\n",
              CONFIG_CONTEST2026_441_VISION_BADGE_MIMO_API_KEY_PATH, -ch);
      return ch;
    }

  length = fread(stored, 1, sizeof(stored), file);
  ch = fgetc(file);
  too_long = ch != EOF;
  if (ferror(file) != 0)
    {
      ch = errno;
      fclose(file);
#ifdef __NuttX__
      if (mounted)
        {
          (void)umount("/mnt/sdnand");
        }
#endif

      return -ch;
    }

  if (fclose(file) != 0)
    {
      ch = errno;
#ifdef __NuttX__
      if (mounted)
        {
          (void)umount("/mnt/sdnand");
        }
#endif
      return -ch;
    }

#ifdef __NuttX__
  if (mounted)
    {
      (void)umount("/mnt/sdnand");
    }
#endif

  if (length >= 4 && memcmp(stored, "VSWP", 4) == 0)
    {
      if (too_long)
        {
          return -EBADMSG;
        }

      ch = mimo_decode_provision_record(stored, length, api_key,
                                        api_key_size);
      if (ch < 0)
        {
          fprintf(stderr, "mimo_http: invalid persisted provision record\n");
          return ch;
        }

      length = strlen(api_key);
    }
  else
    {
      if (too_long || length >= api_key_size)
        {
          return -E2BIG;
        }

      memcpy(api_key, stored, length);
      api_key[length] = '\0';
    }

  while (length > 0 &&
         (api_key[length - 1] == '\n' || api_key[length - 1] == '\r' ||
          api_key[length - 1] == ' ' || api_key[length - 1] == '\t'))
    {
      api_key[--length] = '\0';
    }

  if (length == 0)
    {
      fprintf(stderr, "mimo_http: MiMo key file is empty\n");
      return -ENOENT;
    }

  *hostname = strncmp(api_key, "tp-", 3) == 0 ?
              MIMO_TLS_PLAN_HOST : MIMO_TLS_API_HOST;
  return 0;
}

/* ---- 响应解析 ---- */

int mimo_http_parse_chat_response(struct vision_badge_result_s *result)
{
  cJSON *root;
  cJSON *choices;
  cJSON *choice;
  cJSON *message;
  cJSON *content;
  cJSON *error;
  cJSON *error_message;
  size_t content_size;

  root = cJSON_Parse(result->text);
  if (root == NULL)
    {
      fprintf(stderr, "mimo_http: MiMo response is not valid JSON\n");
      return -EBADMSG;
    }

  error = cJSON_GetObjectItemCaseSensitive(root, "error");
  if (cJSON_IsObject(error))
    {
      error_message = cJSON_GetObjectItemCaseSensitive(error, "message");
      if (cJSON_IsString(error_message) && error_message->valuestring != NULL)
        {
          fprintf(stderr, "mimo_http: MiMo API error: %.160s\n",
                  error_message->valuestring);
        }
      else
        {
          fprintf(stderr, "mimo_http: MiMo API returned an error\n");
        }

      cJSON_Delete(root);
      return -EREMOTEIO;
    }

  choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
  choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
  message = cJSON_IsObject(choice) ?
            cJSON_GetObjectItemCaseSensitive(choice, "message") : NULL;
  content = cJSON_IsObject(message) ?
            cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
  if (!cJSON_IsString(content) || content->valuestring == NULL)
    {
      cJSON_Delete(root);
      fprintf(stderr, "mimo_http: MiMo response has no answer text\n");
      return -EBADMSG;
    }

  content_size = strlen(content->valuestring);
  if (content_size >= result->capacity)
    {
      cJSON_Delete(root);
      return -E2BIG;
    }

  memmove(result->text, content->valuestring, content_size + 1);
  result->length = content_size;
  result->direction_hint = 0;
  cJSON_Delete(root);
  return 0;
}

/* ---- HTTP Sink 回调 ---- */

int mimo_http_sink_callback(char **buffer, int offset, int datend,
                            int *buffer_size, void *arg)
{
  struct mimo_http_sink_s *sink = arg;
  struct vision_badge_result_s *result = sink->result;
  size_t size;

  (void)buffer_size;
  if (offset < 0 || datend < offset)
    {
      return -EPROTO;
    }

  size = (size_t)(datend - offset);
  if (result->length >= result->capacity ||
      size > result->capacity - 1 - result->length)
    {
      return -E2BIG;
    }

  memcpy(result->text + result->length, *buffer + offset, size);
  result->length += size;
  result->text[result->length] = '\0';
  return 0;
}

/* ---- 通用 HTTP POST ---- */

int mimo_http_post(const char *hostname, const char *path,
                   const char *api_key,
                   const char *headers[], size_t nheaders,
                   size_t bodylen,
                   int (*body_callback)(void *buffer, size_t *size,
                                        const void **data, size_t requested,
                                        void *arg),
                   void *body_arg,
                   int (*sink_callback)(char **buffer, int offset, int datend,
                                        int *buffer_size, void *arg),
                   void *sink_arg)
{
  struct webclient_context http;
  char authorization[MIMO_HTTP_API_KEY_SIZE +
                     sizeof("Authorization: Bearer ")];
  char url[256];
  char *work_buffer = NULL;
  int ret;

  if (hostname == NULL || path == NULL || api_key == NULL)
    {
      return -EINVAL;
    }

  memset(authorization, 0, sizeof(authorization));

  ret = snprintf(url, sizeof(url), "https://%s%s", hostname, path);
  if (ret < 0 || (size_t)ret >= sizeof(url))
    {
      return -EOVERFLOW;
    }

  ret = snprintf(authorization, sizeof(authorization),
                 "Authorization: Bearer %s", api_key);
  if (ret < 0 || (size_t)ret >= sizeof(authorization))
    {
      ret = -EOVERFLOW;
      goto out;
    }

  work_buffer = malloc(MIMO_HTTP_WORK_SIZE);
  if (work_buffer == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  webclient_set_defaults(&http);
  http.protocol_version = WEBCLIENT_PROTOCOL_VERSION_HTTP_1_1;
  http.method = "POST";
  http.url = url;
  http.buffer = work_buffer;
  http.buflen = MIMO_HTTP_WORK_SIZE;
  http.headers = headers;
  http.nheaders = nheaders;
  http.bodylen = bodylen;
  http.body_callback = body_callback;
  http.body_callback_arg = body_arg;
  http.sink_callback = sink_callback;
  http.sink_callback_arg = sink_arg;
  http.tls_ops = &g_mimo_tls_ops;
  http.timeout_sec = MIMO_HTTP_TIMEOUT_SECONDS;

  ret = webclient_perform(&http);
  if (ret < 0)
    {
      fprintf(stderr, "mimo_http: HTTP request failed: %d\n", ret);
      goto out_work;
    }

  printf("mimo_http: HTTP status=%d\n", http.http_status);
  if (http.http_status < 200 || http.http_status >= 300)
    {
      ret = -EREMOTEIO;
      goto out_work;
    }

out_work:
  free(work_buffer);
out:
  memset(authorization, 0, sizeof(authorization));
  return ret;
}

int mimo_http_prepare_session(void)
{
  int ret;

#ifdef __NuttX__
  ret = nxmutex_lock(&g_mimo_session_lock);
  if (ret < 0)
    {
      return ret;
    }
#else
  ret = 0;
#endif

  ret = mimo_http_prepare_network();
  if (ret == 0 || ret == -ENOSYS)
    {
      ret = mimo_http_sync_clock();
    }

#ifdef __NuttX__
  nxmutex_unlock(&g_mimo_session_lock);
#endif
  return ret;
}

/* ---- 网络检查 ---- */

int mimo_http_network_check(void)
{
  struct mimo_tls_context_s *tls;
  char error[128];
  char response[MIMO_HTTP_HEADER_SIZE];
  int ret;
  int i;

  ret = mimo_http_sync_clock();
  if (ret < 0)
    {
      return ret;
    }

  tls = calloc(1, sizeof(*tls));
  if (tls == NULL)
    {
      return -ENOMEM;
    }

  ret = mimo_tls_prepare(tls, MIMO_TLS_API_HOST, 10000, false);
  if (ret == 0)
    {
      printf("mimo_http: connecting to https://%s%s\n",
             MIMO_TLS_API_HOST, MIMO_TLS_PATH);
      ret = mimo_tls_handshake(tls, MIMO_TLS_API_HOST, MIMO_TLS_PORT);
    }

  if (ret == 0)
    {
      printf("mimo_http: certificate and hostname verified; %s / %s\n",
             mbedtls_ssl_get_version(&tls->ssl),
             mbedtls_ssl_get_ciphersuite(&tls->ssl));
      ret = mimo_tls_read_http_headers(tls, MIMO_TLS_API_HOST, response,
                                       sizeof(response));
    }

  if (ret != 0)
    {
      mbedtls_strerror(ret, error, sizeof(error));
      fprintf(stderr,
              "mimo_http: secure network check failed: %d (%s)\n",
              ret, error);
    }
  else
    {
      for (i = 0; response[i] != '\0'; i++)
        {
          if (response[i] == '\r' || response[i] == '\n')
            {
              response[i] = '\0';
              break;
            }
        }

      printf("mimo_http: HTTPS response: %s\n", response);
    }

  mimo_tls_free(tls);
  free(tls);
  return ret;
}

#else /* !CONFIG_CRYPTO_MBEDTLS */

int mimo_http_load_credentials(char *api_key, size_t api_key_size,
                               const char **hostname)
{
  (void)api_key;
  (void)api_key_size;
  (void)hostname;
  return -ENOSYS;
}

int mimo_http_prepare_network(void)
{
  return -ENOSYS;
}

int mimo_http_prepare_session(void)
{
  return -ENOSYS;
}

int mimo_http_sync_clock(void)
{
  return -ENOSYS;
}

int mimo_http_parse_chat_response(struct vision_badge_result_s *result)
{
  (void)result;
  return -ENOSYS;
}

int mimo_http_post(const char *hostname, const char *path,
                   const char *api_key,
                   const char *headers[], size_t nheaders,
                   size_t bodylen,
                   int (*body_callback)(void *buffer, size_t *size,
                                        const void **data, size_t requested,
                                        void *arg),
                   void *body_arg,
                   int (*sink_callback)(char **buffer, int offset, int datend,
                                        int *buffer_size, void *arg),
                   void *sink_arg)
{
  (void)hostname;
  (void)path;
  (void)api_key;
  (void)headers;
  (void)nheaders;
  (void)bodylen;
  (void)body_callback;
  (void)body_arg;
  (void)sink_callback;
  (void)sink_arg;
  return -ENOSYS;
}

int mimo_http_network_check(void)
{
  return -ENOSYS;
}

#endif /* CONFIG_CRYPTO_MBEDTLS */

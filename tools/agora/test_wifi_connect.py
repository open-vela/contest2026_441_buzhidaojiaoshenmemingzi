#!/usr/bin/env python3
"""Exercise actual AP connect/config/diagnostic functions on Linux.

Vendor APIs, radio and clocks are mocked; this is NOT a WPA handshake test.
The native host structure layout is NOT proof of the Cortex-M archive ABI.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "chips/bk7258/common/bk7258_wifi_control.c").read_text()


def function(signature):
    start = SOURCE.index(signature)
    opening = SOURCE.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


def structure(name):
    start = SOURCE.index("struct " + name + "\n{")
    end = SOURCE.index("};", start) + 2
    return SOURCE[start:end]


header = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <syslog.h>
#include <time.h>
#define FAR
#define OK 0
#define CONFIG_BK7258_AP_CORE 1
#define BK7258_WIFI_SSID_MAX_LEN 32
#define BK7258_WIFI_PASSWORD_MAX_LEN 63
#define BK7258_WIFI_CONTROL_POLL_MS 100
#define BK7258_WIFI_SECURITY_AUTO 12
#define BK7258_WIFI_LINK_CONNECTED 3
#define MSEC2TICK(x) (x)
#define TICK2MSEC(x) (x)
struct bk7258_wifi_control_wire_s {
 uint32_t ssid_len, password_len, timeout_ms;
 char ssid[33], password[64];
};
struct bk7258_wifi_result_s { uint32_t link_state, ipaddr; };
'''
stubs = r'''
static clock_t tick;
static bool active, never_connect, stale_link, cancelled;
static int stop_error, set_error, verify_error, start_error, sync_error, reason_error;
static int mismatch, reason_calls, starts, stops;
static struct bk7258_wifi_sta_config_s submitted;
static char sequence[100], logs[1024];
static void event(char c) { size_t n=strlen(sequence); assert(n+1<sizeof(sequence)); sequence[n]=c; sequence[n+1]=0; }
static void test_syslog(int priority, const char *format, ...) __attribute__((format(printf,2,3)));
static void test_syslog(int priority, const char *format, ...)
{
 (void)priority; va_list ap;va_start(ap,format);
 size_t n=strlen(logs);vsnprintf(logs+n,sizeof(logs)-n,format,ap);va_end(ap);
}
#define syslog test_syslog
static clock_t clock_systime_ticks(void) { return tick; }
static int nxsig_usleep(unsigned us) { tick+=us/1000;return 0; }
static int bk7258_wifi_vendor_result(int ret) { return ret==0?0:ret<0?ret:-EIO; }
static bool bk7258_wifi_password_length_valid(size_t n) { return !n || (n>=8 && n<=63); }
static bool bk7258_wifi_local_cancelled(void) { return cancelled; }
static int bk7258_wifi_stop_sta(void) { event('S');stops++;active=false;return stop_error; }
static int bk7258_wifi_read_link(struct bk7258_wifi_result_s *out)
{
 out->link_state=stale_link || (active && !never_connect)?3:2;
 out->ipaddr=out->link_state==3?1:0;return 0;
}
static int bk7258_wifi_sync_native_link(struct bk7258_wifi_result_s *out)
{ (void)out;event('N');return active?sync_error:0; }
static int bk_wifi_sta_set_config(const struct bk7258_wifi_sta_config_s *cfg)
{
 event('C');submitted=*cfg;assert(cfg->no_auto_fci==1);
 assert(cfg->security==12 && cfg->channel==0 && cfg->user_fast_connect==0);
 for(size_t i=0;i<sizeof(cfg->bssid);i++) assert(!cfg->bssid[i]);
 for(size_t i=0;i<sizeof(cfg->psk);i++) assert(!cfg->psk[i]);
 for(size_t i=0;i<sizeof(cfg->reserved);i++) assert(!cfg->reserved[i]);
 return set_error;
}
static int bk_wifi_sta_get_config(struct bk7258_wifi_sta_config_s *actual)
{
 event('V');*actual=submitted;
 if(mismatch==1) actual->ssid[0]='?';
 if(mismatch==2) actual->password[0]='?';
 if(mismatch==3) memset(actual->password,'x',sizeof(actual->password));
 return verify_error;
}
static int bk_wifi_sta_start(void) { event('B');starts++;active=true;return start_error; }
static int bk_wifi_sta_get_linkstate_with_reason(struct bk7258_wifi_link_reason_s *out)
{ assert(active);event('R');reason_calls++;out->state=2;out->reason=15;return reason_error; }
'''
main = r'''
static void reset(void)
{
 tick=0;active=never_connect=stale_link=cancelled=false;
 stop_error=set_error=verify_error=start_error=sync_error=reason_error=0;
 mismatch=reason_calls=starts=stops=0;sequence[0]=logs[0]=0;
 memset(&submitted,0,sizeof(submitted));
}
int main(void)
{
 struct bk7258_wifi_control_wire_s req={.ssid_len=8,.password_len=8,.timeout_ms=300};
 struct bk7258_wifi_result_s result={0};
 strcpy(req.ssid,"mock-net");strcpy(req.password,"mock-key");
 reset();assert(bk7258_wifi_connect(&req,&result)==0);
 assert(starts==1 && stops==1 && !strcmp(sequence,"SNCVBN"));
 assert(!strstr(logs,req.ssid) && !strstr(logs,req.password));
 for(int i=1;i<=3;i++) {
  reset();mismatch=i;assert(bk7258_wifi_connect(&req,&result)==-EIO);
  assert(starts==0 && !strchr(sequence,'B') && strstr(logs,"match=0"));
 }
 reset();verify_error=-ETIMEDOUT;assert(bk7258_wifi_connect(&req,&result)==-ETIMEDOUT && !starts);
 reset();set_error=-EINVAL;assert(bk7258_wifi_connect(&req,&result)==-EINVAL && !strchr(sequence,'V'));
 reset();stop_error=-EBUSY;assert(bk7258_wifi_connect(&req,&result)==-EBUSY && !strchr(sequence,'C'));
 reset();start_error=-EIO;assert(bk7258_wifi_connect(&req,&result)==-EIO);
 assert(starts==1 && reason_calls==0 && !strchr(sequence,'R'));
 reset();stale_link=true;assert(bk7258_wifi_connect(&req,&result)==-ETIMEDOUT && !starts);
 reset();never_connect=true;assert(bk7258_wifi_connect(&req,&result)==-ETIMEDOUT);
 assert(reason_calls==1 && stops==2 && strchr(sequence,'R')<strrchr(sequence,'S'));
 assert(strstr(logs,"reason=15") && strstr(logs,"ret=-110"));
 reset();never_connect=true;reason_error=-EIO;
 assert(bk7258_wifi_connect(&req,&result)==-ETIMEDOUT && reason_calls==1);
 assert(strstr(logs,"query=-5") && !strstr(logs,"reason=15"));
 reset();sync_error=-ENETDOWN;assert(bk7258_wifi_connect(&req,&result)==-ENETDOWN && reason_calls==1);
 reset();cancelled=true;assert(bk7258_wifi_connect(&req,&result)==-ECANCELED && !starts && !stops);
 reset();req.ssid_len=0;assert(bk7258_wifi_connect(&req,&result)==-EINVAL && !stops);
 req.ssid_len=8;
 reset();req.password_len=64;assert(bk7258_wifi_connect(&req,&result)==-EINVAL && !stops);
 req.password_len=8;
 reset();req.password[8]='x';assert(bk7258_wifi_connect(&req,&result)==-EINVAL && !stops);
 req.password[8]=0;
 reset();req.password_len=0;req.password[0]=0;assert(bk7258_wifi_connect(&req,&result)==0);
 puts("PASS: actual connect/config/readback/reason ordering; no credentials logged; radio mocked");
 return 0;
}
'''
program = (header + structure("bk7258_wifi_sta_config_s") + "\n" +
           structure("bk7258_wifi_link_reason_s") + stubs +
           function("static int bk7258_wifi_verify_sta_config(") +
           function("static void bk7258_wifi_connect_diagnostic(") +
           function("static int bk7258_wifi_connect(") + main)
with tempfile.TemporaryDirectory(prefix="r1-wifi-connect-") as directory:
    source = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    source.write_text(program)
    subprocess.run([os.environ.get("CC", "cc"), "-D_DEFAULT_SOURCE", "-std=c11",
                    "-Wall", "-Wextra", "-Werror", "-g", "-O1", "-no-pie",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

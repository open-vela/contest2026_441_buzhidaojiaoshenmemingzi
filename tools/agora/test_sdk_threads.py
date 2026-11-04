#!/usr/bin/env python3
"""Compile the actual SDK thread bridge for CP/AP with scheduler-only mocks.

No vendor cache, radio, key derivation or real NuttX scheduling is exercised.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "chips/bk7258/common/bk7258_os_adapt.c").read_text()


def function(signature):
    start = SOURCE.index(signature)
    opening = SOURCE.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


start = SOURCE.index("struct bk7258_sdk_thread_start_s\n{")
structure = SOURCE[start:SOURCE.index("};", start) + 2]
functions = "\n".join(function(signature) for signature in (
    "static bool bk7258_sdk_psk_name(",
    "static bool bk7258_sdk_psk_handle(",
    "static bool bk7258_sdk_psk_alive(",
    "static int bk7258_sdk_thread_trampoline(",
    "bk_err_t rtos_create_thread(",
    "bk_err_t rtos_thread_set_priority(",
    "bk_err_t rtos_delete_thread(",
))

header = r'''
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <syslog.h>
#include <sched.h>
#define FAR
#define OK 0
#define BK_OK 0
#define BK_FAIL -1
#define CONFIG_TASK_NAME_SIZE 31
#define SCHED_PRIORITY_DEFAULT 100
#define BK7258_SDK_THREAD_ARG_BUFSIZE (2u + sizeof(uintptr_t) * 2u + 1u)
#ifdef CONFIG_BK7258_AP_CORE
#define BK7258_SDK_ARG_THREAD_NAME "dvp_work_thread"
#else
#define BK7258_SDK_ARG_THREAD_NAME "tempd"
#endif
typedef int bk_err_t;
typedef void *beken_thread_t;
typedef void *beken_thread_arg_t;
typedef void (*beken_thread_function_t)(void *);
typedef int (*main_t)(int, char **);
extern beken_thread_t wpa_pskcalc_thread_handle __attribute__((weak));
struct tcb_s { char name[32]; };
static struct tcb_s tcb;
static int allocations, frees, creates, calls, priority_calls, locks, deletes;
static int create_result, priority_result, last_priority;
static pid_t current_pid, alive_pid, deleted_pid;
static bool fail_alloc, immediate;
static main_t entry;
static void *seen_arg;
static char context[100], name[32], logs[4096];
static void log_message(int level, const char *fmt, ...)
{
 (void)level;
 va_list args;va_start(args,fmt);
 size_t used=strlen(logs);
 vsnprintf(logs+used,sizeof(logs)-used,fmt,args);va_end(args);
}
static void *fake_alloc(size_t size)
{ if(fail_alloc)return NULL;void *p=malloc(size);if(p)allocations++;return p; }
static void fake_free(void *p) { assert(p);frees++;free(p); }
static pid_t gettid_mock(void) { return current_pid; }
#ifndef CONFIG_BK7258_AP_CORE
static int lock_mock(void) { locks++;return 0; }
static int unlock_mock(void) { assert(locks>0);locks--;return 0; }
static struct tcb_s *tcb_mock(pid_t pid)
{ assert(locks>0);return pid==alive_pid?&tcb:NULL; }
#endif
static int priority_mock(pid_t pid,const struct sched_param *param)
{ (void)pid;priority_calls++;last_priority=param->sched_priority;return priority_result; }
static int delete_mock(pid_t pid) { deletes++;deleted_pid=pid;return 0; }
static pid_t create_mock(const char *task_name,int priority,int stack,
                         main_t task_entry,char *const argv[])
{
 assert(stack>0);creates++;last_priority=priority;entry=task_entry;
 snprintf(name,sizeof(name),"%s",task_name);
 context[0]=0;
 if(argv){assert(argv[0] && argv[1]==NULL);snprintf(context,sizeof(context),"%s",argv[0]);}
 if(create_result>0 && immediate && argv) {
   char *args[]={name,context,NULL};assert(entry(2,args)==0);
 }
 return create_result;
}
#define syslog log_message
#define wlerr(...) log_message(LOG_ERR,__VA_ARGS__)
#define kmm_malloc fake_alloc
#define kmm_free fake_free
#define nxsched_gettid gettid_mock
#define nxsched_get_tcb tcb_mock
#define sched_lock lock_mock
#define sched_unlock unlock_mock
#define sched_setparam priority_mock
#define task_delete delete_mock
#define kthread_create create_mock
'''

main = r'''
static void worker(void *arg) { calls++;seen_arg=arg; }
static void reset(void)
{
 assert(allocations==frees && locks==0);
 allocations=frees=creates=calls=priority_calls=deletes=0;
 create_result=42;priority_result=0;current_pid=42;alive_pid=42;
 fail_alloc=immediate=false;seen_arg=NULL;entry=NULL;logs[0]=0;
 strcpy(tcb.name,"pskc");wpa_pskcalc_thread_handle=NULL;
}
int main(void)
{
 beken_thread_t thread=NULL;
#ifdef TEST_NO_PSK_HANDLE
 (void)reset;
 assert(!bk7258_sdk_psk_handle(&thread));
 create_result=42;
 assert(rtos_create_thread(&thread,5,"generic",worker,2048,NULL)==BK_OK);
 assert((uintptr_t)thread==42 && creates==1 && allocations==0);
 assert(rtos_thread_set_priority(&thread,5)==BK_OK && logs[0]==0);
 assert(rtos_delete_thread(&thread)==BK_OK);
 puts("PASS: absent weak vendor handle keeps generic SDK bridge unchanged");
 return 0;
#else
 reset();assert(rtos_create_thread(&thread,5,NULL,worker,2048,NULL)==BK_FAIL && creates==0);
 assert(rtos_create_thread(&thread,5,"generic",NULL,2048,NULL)==BK_FAIL);
 assert(rtos_create_thread(&thread,5,"generic",worker,0,NULL)==BK_FAIL);
 assert(rtos_create_thread(&thread,5,"other",worker,2048,(void *)1)==BK_FAIL && creates==0);
 reset();assert(rtos_create_thread(&thread,5,"generic",worker,2048,NULL)==BK_OK);
 assert((uintptr_t)thread==42 && allocations==0 && context[0]==0 && logs[0]==0);
 reset();assert(rtos_create_thread(&thread,5,"pskc",worker,2048,(void *)1)==BK_FAIL);
 reset();immediate=true;
 assert(rtos_create_thread(&thread,5,"pskc",worker,2048,NULL)==BK_OK);
 assert((uintptr_t)thread==42 && last_priority==97);
#ifndef CONFIG_BK7258_AP_CORE
 assert(allocations==1 && frees==1 && calls==1 && seen_arg==NULL);
 assert(entry==bk7258_sdk_thread_trampoline);
 assert(strstr(logs,"PSK thread create") && strstr(logs,"PSK thread running") &&
        strstr(logs,"PSK thread returned") && strstr(logs,"PSK thread created"));
#else
 assert(allocations==0 && calls==0 && context[0]==0 && logs[0]==0);
#endif
 reset();immediate=true;
 assert(rtos_create_thread(&thread,5,BK7258_SDK_ARG_THREAD_NAME,worker,2048,(void *)123)==BK_OK);
 assert(allocations==1 && frees==1 && calls==1 && seen_arg==(void *)123 && logs[0]==0);
 reset();create_result=-ENOMEM;
 assert(rtos_create_thread(&thread,5,BK7258_SDK_ARG_THREAD_NAME,worker,2048,(void *)123)==BK_FAIL);
 assert(allocations==1 && frees==1);
 reset();fail_alloc=true;
 assert(rtos_create_thread(&thread,5,BK7258_SDK_ARG_THREAD_NAME,worker,2048,(void *)123)==BK_FAIL);
 assert(creates==0 && allocations==0);
 reset();assert(bk7258_sdk_thread_trampoline(0,NULL)==-EINVAL);
 char *bad[]={"pskc","garbage",NULL};
 assert(bk7258_sdk_thread_trampoline(2,bad)==-EINVAL);
 reset();wpa_pskcalc_thread_handle=(void *)42;
 assert(rtos_thread_set_priority(&wpa_pskcalc_thread_handle,5)==BK_OK && last_priority==97);
#ifndef CONFIG_BK7258_AP_CORE
 assert(strstr(logs,"alive=1 ret=0"));
#else
 assert(logs[0]==0);
#endif
 reset();wpa_pskcalc_thread_handle=(void *)99;alive_pid=42;priority_result=-1;
 assert(rtos_thread_set_priority(&wpa_pskcalc_thread_handle,5)==BK_FAIL);
 assert((uintptr_t)wpa_pskcalc_thread_handle==99 && priority_calls==1 && creates==0);
#ifndef CONFIG_BK7258_AP_CORE
 assert(strstr(logs,"pid=99 alive=0 ret=-1"));
#else
 assert(logs[0]==0);
#endif
 reset();wpa_pskcalc_thread_handle=(void *)42;strcpy(tcb.name,"other");
 assert(rtos_thread_set_priority(&wpa_pskcalc_thread_handle,5)==BK_OK);
#ifndef CONFIG_BK7258_AP_CORE
 assert(strstr(logs,"alive=0")); /* PID reuse is not a live PSK worker. */
#endif
 reset();assert(rtos_thread_set_priority(NULL,5)==BK_OK && logs[0]==0);
 reset();assert(rtos_delete_thread(NULL)==BK_OK && deletes==1 && deleted_pid==0);
#ifndef CONFIG_BK7258_AP_CORE
 assert(strstr(logs,"PSK thread delete pid=42 self=1"));
#else
 assert(logs[0]==0);
#endif
 assert(locks==0);
 puts("PASS: actual CP/AP SDK thread bridge, PSK-only tracing, argument boundaries, create failures, live/stale PID and deletion diagnostics; scheduler/vendor behavior mocked");
 return 0;
#endif
}
'''

with tempfile.TemporaryDirectory(prefix="r1-sdk-threads-") as directory:
    path = Path(directory) / "test.c"
    handle = Path(directory) / "handle.c"
    binary = Path(directory) / "test"
    path.write_text(header + structure + functions + main)
    # Separate translation unit matches the weak declaration resolved by
    # the immutable CP archive, without the compiler folding its address.
    handle.write_text("void *wpa_pskcalc_thread_handle;\n")
    for defines, handle_objects in (
        ([], [str(handle)]),
        (["-DCONFIG_BK7258_AP_CORE=1"], [str(handle)]),
        (["-DTEST_NO_PSK_HANDLE=1"], []),
    ):
        subprocess.run([os.environ.get("CC", "cc"), "-D_DEFAULT_SOURCE",
                        "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
                        # Existing generic SDK no-argument function cast is
                        # preserved and captured, not executed, by this test.
                        "-Wno-cast-function-type",
                        "-no-pie", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all", *defines, str(path),
                        *handle_objects, "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

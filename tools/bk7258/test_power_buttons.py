#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real candidate key poll/queue/action worker, with GPIO/RPC/scheduler mocked."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
source = (repo / "app/vision_badge/src/vision_badge_rpc.c").read_text()
def function(signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

program = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <setjmp.h>
#define CONFIG_CONTEST2026_441_POWER 1
#define BUTTON_K1 1u
#define BUTTON_K2 2u
#define BUTTON_K3 4u
#define TICK2MSEC(x) (x)
struct vision_badge_rpc_result_s { int status,stage; char text[256]; };
static unsigned int g_key_pending;
static bool g_key_busy;
static int g_key_wake;
static unsigned int trace[90], step, trace_count;
static int scenes, ups, voices, offs;
static bool poll_inside_rpc;
static jmp_buf poll_finished, action_finished;
static void run_trace(const unsigned int*,unsigned int);
static void nxsem_post(int *s) { (*s)++; }
static int nxsem_wait_uninterruptible(int *s) {
  if (!*s) longjmp(action_finished,1);
  (*s)--; return 0;
}
static unsigned int board_buttons(void) { return trace[step]; }
static clock_t clock_systime_ticks(void) { return (step+1)*50; }
static void nxsig_usleep(unsigned int us) {
  assert(us==50000);
  if (++step==trace_count) longjmp(poll_finished,1);
}
static int vision_badge_rpc_power(const char *a,struct vision_badge_rpc_result_s *r)
{ assert(!strcmp(a,"toggle") && g_key_busy); offs++; r->status=0; return 0; }
static int vision_badge_rpc_audio_diagnostic(const char *a,struct vision_badge_rpc_result_s *r)
{
  assert(g_key_busy); r->status=0;
  if(!strcmp(a,"volume down")) scenes++;
  else {assert(!strcmp(a,"volume up"));ups++;}
  if (poll_inside_rpc) {
    unsigned int values[67]={0};
    for(unsigned int i=1;i<=62;i++)values[i]=BUTTON_K2;
    values[64]=values[65]=BUTTON_K1;
    int before=offs;
    run_trace(values,67); /* Hold/release middle, then short left during RPC. */
    assert(offs==before && g_key_pending==4u); /* Poll never calls RPC. */
  }
  return 0;
}
static int vision_badge_rpc_conversation(const char *a,struct vision_badge_rpc_result_s *r)
{ assert(!strcmp(a,"toggle") && g_key_busy); voices++; r->status=0; return 0; }
''' + function("static void vision_badge_button_queue(") + function("static int vision_badge_button_action_worker(") + function("static void vision_badge_button_event(") + function("static int vision_badge_button_worker(") + r'''
static void run_trace(const unsigned int *values,unsigned int count) {
  assert(count<=90); memcpy(trace,values,count*sizeof(*values)); step=0; trace_count=count;
  if (!setjmp(poll_finished)) vision_badge_button_worker(0,NULL);
}
static void drain(void) {
  if (!setjmp(action_finished)) vision_badge_button_action_worker(0,NULL);
  assert(!g_key_busy && !g_key_pending && !g_key_wake);
}
int main(void) {
  vision_badge_button_event(BUTTON_K1,49); vision_badge_button_event(BUTTON_K2,49);
  vision_badge_button_event(BUTTON_K1,3000); vision_badge_button_event(BUTTON_K3,3000);
  vision_badge_button_event(BUTTON_K1|BUTTON_K2,5000);
  assert(!g_key_pending && !g_key_wake);
  vision_badge_button_event(BUTTON_K1,50); assert(!scenes); drain(); assert(scenes==1);
  vision_badge_button_event(BUTTON_K3,100);drain();assert(ups==1);
  vision_badge_button_event(BUTTON_K2,2999); drain(); assert(voices==1);
  vision_badge_button_event(BUTTON_K2,3000); vision_badge_button_event(BUTTON_K2,12000);
  drain(); assert(offs==1); /* Repeated requests coalesce before dispatch. */
  const unsigned int boot[]={BUTTON_K2,BUTTON_K2,0};
  const unsigned int chord[]={0,BUTTON_K1,BUTTON_K1|BUTTON_K2,BUTTON_K2,0};
  run_trace(boot,3);run_trace(chord,5);drain();assert(scenes==1 && voices==1 && offs==1);
  unsigned int hold[67]={0};
  for(unsigned int i=1;i<=62;i++) hold[i]=BUTTON_K2;
  run_trace(hold,63); /* Still physically held: off must already be queued. */
  assert(g_key_pending==4u);drain();assert(offs==2 && voices==1);
  const unsigned int right[]={0,BUTTON_K3,BUTTON_K3,0};
  run_trace(right,4);drain();assert(ups==2);
  poll_inside_rpc=true;
  vision_badge_button_event(BUTTON_K1,100);drain();
  assert(scenes==2 && voices==1 && offs==3);
  puts("POWER_BUTTONS_PASS actual poll/async queue: bounds, held boot/chord, no RPC in poll, off priority, stale work dropped; GPIO/RPC mocked");
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="r1-power-buttons-") as directory:
    path = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    path.write_text(program)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-no-pie",
                    str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

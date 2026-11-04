#!/usr/bin/env python3
"""Compile the real RPC validator/CP packing path with transport-only stubs.

This checks command acceptance and payload boundaries, not real RPMsg or RTC.
"""
import os
from pathlib import Path
import subprocess
import tempfile

PROJECT = Path(__file__).resolve().parents[2]
SOURCE = (PROJECT / "app/vision_badge/src/vision_badge_rpc.c").read_text()


def function(signature):
    start = SOURCE.index(signature)
    opening = SOURCE.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


definitions = SOURCE[SOURCE.index("enum vision_badge_rpc_command_e"):
                     SOURCE.index("static bool vision_badge_rpc_endpoint_ready")]
header = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <syslog.h>
#define VISION_BADGE_RPC_TEXT_SIZE 256
#define VISION_BADGE_RPC_PROMPT_SIZE 160
#define VISION_BADGE_RPC_MAGIC 0x47444256u
#define VISION_BADGE_RPC_VERSION 1u
#define VISION_BADGE_RPC_SEND_MS 200u
#define BK7258_RPTUN_STATE_CONNECTED 4
#define NXMUTEX_INITIALIZER 0
#define MSEC2TICK(x) (x)
#define CONFIG_CONTEST2026_441_AGORA 1
#define CONFIG_CONTEST2026_441_VISION_BADGE_JPEG_BUFFER_SIZE 262144
#define VISION_BADGE_STAGE_CAPTURE 1
#define VISION_BADGE_STAGE_QUERY 2
#define VISION_BADGE_STAGE_DONE 4
#define VISION_BADGE_STAGE_ERROR 5
static unsigned int activity_seen;
static void feedback_service_activity(int stage) { activity_seen |= 1u << stage; }
struct vision_badge_image_s { void *data; size_t capacity, size; unsigned width, height; };
typedef int sem_t;
typedef int mutex_t;
struct rpmsg_endpoint { int unused; };
struct vision_badge_rpc_result_s { int status; int stage; char text[256]; };
struct bk7258_rptun_control_s { uint32_t generation; int state; };
'''
stubs = r'''
static char received[264];
static int capture_calls, scene_calls, capture_error, pair_calls, save_calls;
static int camera_service_capture(struct vision_badge_image_s *image)
{ capture_calls++; image->width=640; image->height=480; image->size=22000; return capture_error; }
static int camera_service_capture_pair(struct vision_badge_image_s *early, struct vision_badge_image_s *late)
{ pair_calls++; early->size=20000; late->size=24000; return capture_error; }
static int camera_service_control(const char *action, char *text, size_t capacity)
{ strcpy(received,action);snprintf(text,capacity,"camera-control");return 0; }
static int agora_control_capture(const struct vision_badge_image_s *image, char *text, size_t capacity)
{ assert(image->size>0); save_calls++; snprintf(text, capacity, "saved"); return 0; }
static int agora_control_scene(const struct vision_badge_image_s *image, char *text, size_t capacity)
{ (void)image; scene_calls++; snprintf(text, capacity, "scene-reply"); return 0; }
static int vision_badge_battery_status(char *text, size_t capacity)
{ snprintf(text, capacity, "battery-reply"); return 0; }
static int vision_badge_audio_diagnostic(const char *action, char *text, size_t capacity)
{ strcpy(received, action); snprintf(text, capacity, "audio-diag-reply"); return 0; }
static int agora_service_command(const char *action, char *text, size_t capacity)
{
  assert(strlen(action) < sizeof(received));
  strcpy(received, action);
  snprintf(text, capacity, "service-reply");
  return 0;
}
static struct vision_badge_rpc_wire_s sent;
static struct bk7258_rptun_control_s control = { 1, 4 };
static struct bk7258_rptun_control_s *bk7258_rptun_control(void) { return &control; }
static int vision_badge_rpc_initialize(void) { return 0; }
static bool vision_badge_rpc_endpoint_ready(void) { return true; }
static clock_t clock_systime_ticks(void) { return 0; }
static int nxmutex_lock(mutex_t *m) { (void)m; return 0; }
static void nxmutex_unlock(mutex_t *m) { (void)m; }
static int nxsem_trywait(sem_t *s) { (void)s; return -1; }
static void nxsig_usleep(unsigned int us) { (void)us; }
static int nxsem_tickwait_uninterruptible(sem_t *s, clock_t t)
{ (void)s; (void)t; g_vision_badge_rpc.reply_valid = true; return 0; }
static int vision_badge_rpc_send(const struct vision_badge_rpc_wire_s *m, unsigned int t)
{ (void)t; sent = *m; return 0; }
'''
main = r'''
int main(void)
{
  struct vision_badge_rpc_wire_s wire = { .magic=VISION_BADGE_RPC_MAGIC, .version=1 };
  struct vision_badge_rpc_result_s result;
  char config[264] = "config ";
  for (unsigned int command=1; command<=10; command++)
    { wire.command=command; assert(vision_badge_rpc_wire_valid(&wire, sizeof(wire))); }
  wire.command=11; assert(!vision_badge_rpc_wire_valid(&wire, sizeof(wire)));
  wire.command=7; wire.version=2; assert(!vision_badge_rpc_wire_valid(&wire, sizeof(wire)));
  wire.version=1; wire.magic=0; assert(!vision_badge_rpc_wire_valid(&wire, sizeof(wire)));
  wire.magic=VISION_BADGE_RPC_MAGIC;
  assert(!vision_badge_rpc_wire_valid(&wire, sizeof(wire)-1));
  assert(!vision_badge_rpc_wire_valid(NULL, sizeof(wire)));
  assert(vision_badge_rpc_agora("version", &result)==0);
  assert(sent.command==7 && !strcmp(sent.prompt, "version") && !sent.text[0]);
  assert(ap_dispatch(sent)==0 && !strcmp(received, "version"));
  assert(vision_badge_rpc_battery(&result)==0);
  assert(sent.command==8 && !strcmp(sent.prompt, "status") && !sent.text[0]);
  assert(ap_dispatch(sent)==0);
  assert(vision_badge_rpc_audio_diagnostic("tone 30", &result)==0);
  assert(sent.command==9 && !strcmp(sent.prompt, "tone 30"));
  assert(ap_dispatch(sent)==0 && !strcmp(received, "tone 30"));
  assert(vision_badge_rpc_audio_diagnostic(NULL, &result)==-EINVAL);
  assert(vision_badge_rpc_camera("capture", &result)==0 && sent.command==10);
  assert(ap_dispatch(sent)==0 && capture_calls==1 && scene_calls==0);
  assert(activity_seen==((1u<<1)|(1u<<4))); activity_seen=0;
  assert(vision_badge_rpc_camera("scene", &result)==0);
  assert(ap_dispatch(sent)==0 && capture_calls==2 && scene_calls==1);
  assert(activity_seen==((1u<<1)|(1u<<2)|(1u<<4))); activity_seen=0;
  capture_error=-EIO;
  assert(ap_dispatch(sent)==-EIO && scene_calls==1);
  assert(activity_seen==((1u<<1)|(1u<<5))); activity_seen=0;
  assert(vision_badge_rpc_camera("bad", &result)==0);
  assert(ap_dispatch(sent)==-EINVAL && capture_calls==3);
  assert(!activity_seen);
  capture_error=0;
  assert(vision_badge_rpc_camera("compare", &result)==0);
  assert(ap_dispatch(sent)==0 && pair_calls==1 && save_calls==2 && scene_calls==1);
  capture_error=-EIO;
  assert(ap_dispatch(sent)==-EIO && pair_calls==2 && save_calls==2);
  for(unsigned i=0;i<3;i++) {
    const char *actions[]={"start","stop","status"};
    assert(vision_badge_rpc_camera(actions[i],&result)==0);
    assert(ap_dispatch(sent)==0 && !strcmp(received,actions[i]));
  }
  memset(config+7, 'x', 200); config[207]=0;
  assert(vision_badge_rpc_agora(config, &result)==0);
  assert(!strcmp(sent.prompt, "config") && strlen(sent.text)==200);
  assert(!strcmp(sent.text, config+7));
  assert(ap_dispatch(sent)==0 && !strcmp(received, config));
  memset(config+7, 'x', 255); config[262]=0;
  assert(vision_badge_rpc_agora(config, &result)==0 && strlen(sent.text)==255);
  assert(ap_dispatch(sent)==0 && !strcmp(received, config));
  config[262]='x'; config[263]=0;
  assert(vision_badge_rpc_agora(config, &result)==-E2BIG);
  assert(vision_badge_rpc_agora(NULL, &result)==-EINVAL);
  assert(sizeof(wire)==440); /* unchanged, below the 496-byte RPMsg payload */
  puts("PASS: ten commands, camera dispatch/error path, real CP packing/AP dispatch, config boundaries; transport/service stubbed");
  return 0;
}
'''
# ARM memory barrier is irrelevant to this host packing test; do not alter source.
exchange = function("static int vision_badge_rpc_exchange(").replace(
    '__asm volatile ("dmb sy" ::: "memory");', '(void)0;')
ap_start = SOURCE.index("if (request.command == VISION_BADGE_RPC_AGORA)")
ap_end = SOURCE.index("else if (request.command == VISION_BADGE_RPC_CONVERSATION)", ap_start)
dispatch = ("static int ap_dispatch(struct vision_badge_rpc_wire_s request) {\n"
            "struct vision_badge_rpc_wire_s response = {0}; struct vision_badge_image_s image; int ret = -EINVAL;\n" +
            SOURCE[ap_start:ap_end] + "return ret; }\n")
program = header + definitions + stubs + function(
    "static bool vision_badge_rpc_wire_valid(") + exchange + function(
    "int vision_badge_rpc_agora(") + function(
    "int vision_badge_rpc_battery(") + function(
    "int vision_badge_rpc_audio_diagnostic(") + function(
    "int vision_badge_rpc_camera(") + dispatch + main
button = function("static int vision_badge_button_worker(")
assert 'BUTTON_K1 | BUTTON_K2' in button
assert 'vision_badge_button_event(keys[i], held_ms)' in button
button_program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <stdbool.h>
#include <time.h>
#include <setjmp.h>
#define BUTTON_K1 1u
#define BUTTON_K2 2u
#define TICK2MSEC(x) (x)
struct vision_badge_rpc_result_s { int status, stage; char text[256]; };
static int scenes, voices;
static unsigned int trace[80], step, trace_count;
static jmp_buf finished;
static unsigned int board_buttons(void) { return trace[step]; }
static clock_t clock_systime_ticks(void) { return (step+1)*50; }
static void nxsig_usleep(unsigned int delay) {
  assert(delay==50000);
  if (++step==trace_count) longjmp(finished,1);
}
static int vision_badge_rpc_camera(const char *action, struct vision_badge_rpc_result_s *r)
{ assert(!strcmp(action,"scene")); scenes++; r->status=0; return 0; }
static int vision_badge_rpc_conversation(const char *action, struct vision_badge_rpc_result_s *r)
{ assert(!strcmp(action,"toggle")); voices++; r->status=0; return 0; }
''' + function("static void vision_badge_button_event(") + button + r'''
static void run_trace(const unsigned int *values, unsigned int count) {
  assert(count<=80); memcpy(trace,values,count*sizeof(*values));
  step=0; trace_count=count;
  if (!setjmp(finished)) vision_badge_button_worker(0,NULL);
}
int main(void) {
  vision_badge_button_event(BUTTON_K1,49);
  vision_badge_button_event(BUTTON_K2,49);
  vision_badge_button_event(BUTTON_K1,3000);
  vision_badge_button_event(BUTTON_K2,5000);
  vision_badge_button_event(BUTTON_K1|BUTTON_K2,100);
  vision_badge_button_event(4u,100);
  assert(!scenes && !voices);
  vision_badge_button_event(BUTTON_K1,50);
  vision_badge_button_event(BUTTON_K2,2999);
  vision_badge_button_event(BUTTON_K2,100);
  assert(scenes==1 && voices==2);
  const unsigned int left[]={0,BUTTON_K1,BUTTON_K1,0};
  const unsigned int middle[]={0,BUTTON_K2,BUTTON_K2,0};
  const unsigned int boot_held[]={BUTTON_K2,BUTTON_K2,0};
  const unsigned int chord[]={0,BUTTON_K1,BUTTON_K1|BUTTON_K2,BUTTON_K2,0};
  run_trace(left,4);run_trace(middle,4);
  assert(scenes==2 && voices==3);
  run_trace(boot_held,3);run_trace(chord,5);
  unsigned int long_hold[65]={0};
  for(unsigned int i=1;i<64;i++)long_hold[i]=BUTTON_K2;
  run_trace(long_hold,65);
  assert(scenes==2 && voices==3);
  puts("PASS: real button worker/event maps left to scene, middle to voice toggle; debounce/long hold/boot-held/chord ignored; GPIO mocked");
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="r1-rpc-") as directory:
    source = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    source.write_text(program)
    subprocess.run([os.environ.get("CC", "cc"), "-D_DEFAULT_SOURCE", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-no-pie",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    # Candidate uses the same packet ABI, with one extra command and an
    # admission gate before every AP resource-owning command.
    power_start = SOURCE.rfind("#ifdef CONFIG_CONTEST2026_441_POWER", 0, ap_start)
    power_dispatch = dispatch.replace(SOURCE[ap_start:ap_end], SOURCE[power_start:ap_end])
    power_stubs = r'''
static bool power_allowed = true;
static int power_calls;
static unsigned int volume_saves;
static unsigned int audio_service_volume_level(void) { return 2; }
static int agora_control_volume_save(unsigned int level) { assert(level==2);volume_saves++;return 0; }
static bool power_service_work_allowed(void) { return power_allowed; }
static int power_service_command(const char *action, char *text, size_t size)
{ power_calls++; snprintf(text,size,"power %s",action); return 0; }
'''
    power_main = main.replace('command<=10', 'command<=11').replace('wire.command=11;', 'wire.command=12;')
    power_main = power_main.replace('  puts("PASS: ten commands,', r'''
  assert(vision_badge_rpc_power(NULL,&result)==-EINVAL);
  assert(vision_badge_rpc_power("invalid",&result)==-EINVAL);
  const char *power_actions[]={"status","quiet","prepare","resume","off","toggle"};
  for (unsigned i=0;i<6;i++) {
    assert(vision_badge_rpc_power(power_actions[i],&result)==0);
    assert(sent.command==11 && !strcmp(sent.prompt,power_actions[i]));
    assert(ap_dispatch(sent)==0);
  }
  assert(power_calls==6);
  assert(volume_saves==0);
  power_allowed=false;
  for (unsigned command=1;command<=10;command++) {
    struct vision_badge_rpc_wire_s request={.command=command};
    strcpy(request.prompt,"start");
    assert(ap_dispatch(request)==(command==VISION_BADGE_RPC_BATTERY?0:-ESHUTDOWN));
  }
  const char *diagnostics[]={"status","stats","visionstats","stop"};
  for (unsigned i=0;i<4;i++) {
    struct vision_badge_rpc_wire_s request={.command=VISION_BADGE_RPC_AGORA};
    strcpy(request.prompt,diagnostics[i]); assert(ap_dispatch(request)==0);
  }
  assert(vision_badge_rpc_power("resume",&result)==0 && ap_dispatch(sent)==0);
  assert(power_calls==7 && sizeof(wire)==440);
  puts("PASS: candidate power admission/CP packing, eleven commands,''')
    source.write_text('#define CONFIG_CONTEST2026_441_POWER 1\n' + header + definitions + stubs + power_stubs +
                      function("static bool vision_badge_rpc_wire_valid(") + exchange +
                      function("int vision_badge_rpc_agora(") + function("int vision_badge_rpc_battery(") +
                      function("int vision_badge_rpc_audio_diagnostic(") + function("int vision_badge_rpc_camera(") +
                      function("int vision_badge_rpc_power(") + power_dispatch + power_main)
    subprocess.run([os.environ.get("CC", "cc"), "-D_DEFAULT_SOURCE", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-no-pie",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    wake_start = SOURCE.rfind("#ifdef CONFIG_CONTEST2026_441_WAKE", 0, SOURCE.index("      bool wake_exclusive="))
    wake_dispatch = dispatch.replace(SOURCE[ap_start:ap_end], SOURCE[wake_start:ap_end]).replace(
        "return ret; }", "wake_service_rpc_end(wake_exclusive); return ret; }")
    wake_stubs = r'''
static int wake_gate_error, wake_begins, wake_ends;
static bool wake_exclusive_seen;
static int wake_service_rpc_begin(bool exclusive) {
  wake_begins++;wake_exclusive_seen=exclusive;return exclusive?wake_gate_error:0;
}
static void wake_service_rpc_end(bool exclusive) {
  assert(exclusive==wake_exclusive_seen);wake_ends++;
}
'''
    wake_main = power_main.replace('  puts("PASS: candidate power', r'''
  power_allowed=true;wake_gate_error=-ETIMEDOUT;
  assert(vision_badge_rpc_camera("capture",&result)==0);
  int before_capture=capture_calls;
  assert(ap_dispatch(sent)==-ETIMEDOUT && capture_calls==before_capture && wake_exclusive_seen);
  const char *wake_actions[]={"wake-start","wake-status","wake-stop"};
  for(unsigned i=0;i<3;i++) {
    assert(vision_badge_rpc_agora(wake_actions[i],&result)==0);
    assert(ap_dispatch(sent)==0 && !wake_exclusive_seen);
  }
  assert(vision_badge_rpc_agora("start",&result)==0);
  assert(ap_dispatch(sent)==-ETIMEDOUT && wake_exclusive_seen);
  const char *volume_actions[]={"volume","volume down","volume up"};
  for(unsigned i=0;i<3;i++) {
    assert(vision_badge_rpc_audio_diagnostic(volume_actions[i],&result)==0);
    assert(ap_dispatch(sent)==0 && !wake_exclusive_seen);
  }
  assert(volume_saves==2); /* Query does not persist; up/down do. */
  assert(vision_badge_rpc_power("status",&result)==0);
  assert(ap_dispatch(sent)==0 && !wake_exclusive_seen);
  assert(vision_badge_rpc_camera("status",&result)==0);
  assert(ap_dispatch(sent)==0 && !wake_exclusive_seen);
  const char *camera_mutations[]={"start","stop","capture"};
  for(unsigned i=0;i<3;i++) {
    assert(vision_badge_rpc_camera(camera_mutations[i],&result)==0);
    assert(ap_dispatch(sent)==-ETIMEDOUT && wake_exclusive_seen);
  }
  int before_power=power_calls;
  assert(vision_badge_rpc_power("off",&result)==0);
  assert(ap_dispatch(sent)==-ETIMEDOUT && power_calls==before_power);
  assert(wake_begins==wake_ends);
  puts("PASS: wake RPC ownership/timeout/always-end, candidate power''')
    source.write_text('#define CONFIG_CONTEST2026_441_POWER 1\n#define CONFIG_CONTEST2026_441_WAKE 1\n' +
                      header + definitions + stubs + power_stubs + wake_stubs +
                      function("static bool vision_badge_rpc_wire_valid(") + exchange +
                      function("int vision_badge_rpc_agora(") + function("int vision_badge_rpc_battery(") +
                      function("int vision_badge_rpc_audio_diagnostic(") + function("int vision_badge_rpc_camera(") +
                      function("int vision_badge_rpc_power(") + wake_dispatch + wake_main)
    subprocess.run([os.environ.get("CC", "cc"), "-D_DEFAULT_SOURCE", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-no-pie",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    source.write_text(button_program)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-no-pie",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

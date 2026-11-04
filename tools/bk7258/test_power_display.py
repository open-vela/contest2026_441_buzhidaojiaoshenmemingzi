#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual panel save/restore and eye request/ack code; LCD/scheduling mocked."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
eye = (repo / "boards/bk7258/aidk_ai_toy/src/bk7258_aidk_eye.c").read_text()
lcd = (repo / "boards/bk7258/aidk_ai_toy/src/bk7258_aidk_dual_lcd.c").read_text()
def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

start = eye.index("      unsigned int sequence =", eye.index("static int aidk_eye_worker("))
end = eye.index("#endif", start)
globals_start = eye.index("static bool g_eye_power_requested")
globals_end = eye.index("extern int bk7258_aidk_dual_lcd_power", globals_start)
globals_text = eye[globals_start:globals_end]
program = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#define NXMUTEX_INITIALIZER 0
#define MSEC2TICK(x) (x)
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
typedef int mutex_t;
struct lcd_dev_s {
  int id;
  int (*getpower)(struct lcd_dev_s*);
  int (*setpower)(struct lcd_dev_s*,int);
};
static int levels[2]={40,60}, set_error[2], get_error[2], sets[2];
static int getpower(struct lcd_dev_s *dev) { return get_error[dev->id] ? -EIO : levels[dev->id]; }
static int setpower(struct lcd_dev_s *dev,int power) {
  sets[dev->id]++; levels[dev->id]=power; return set_error[dev->id];
}
static struct lcd_dev_s devices[2]={{0,getpower,setpower},{1,getpower,setpower}};
static struct {struct lcd_dev_s *lcddev;} g_aidk_lcd_panels[2]={{&devices[0]},{&devices[1]}};
static bool g_aidk_eye_started;
static int g_aidk_eye_wake;
static clock_t ticks;
static int schedule_mode;
static int nxmutex_lock(int *m) { assert(!*m); *m=1; return 0; }
static void nxmutex_unlock(int *m) { assert(*m); *m=0; }
static void nxsem_post(int *s) { (*s)++; }
static int nxsem_wait_uninterruptible(int *s) { (void)s; return 0; }
static clock_t clock_systime_ticks(void) { return ticks; }
static int nxsig_usleep(unsigned int);
''' + globals_text + function(lcd, "int bk7258_aidk_dual_lcd_power(") + '\nstatic void worker_step(void) { for(int once=0;once<1;once++) {\n' + eye[start:end] + '\n}}\n' + r'''
static int nxsig_usleep(unsigned int us) {
  ticks+=us/1000;
  if (schedule_mode==1) worker_step();
  else if (schedule_mode==2) {
    /* A timed-out OFF completes after RESUME is queued. Its old ack must
     * not satisfy the new request. The next wait runs the real owner. */
    assert(g_eye_power_request_sequence==g_eye_power_ack_sequence+2);
    assert(bk7258_aidk_dual_lcd_power(false)==0);
    g_eye_power_ack_sequence++; schedule_mode=1;
  }
  return 0;
}
''' + function(eye, "int bk7258_aidk_eye_power(") + r'''
int main(void) {
  assert(bk7258_aidk_eye_power(false)==-EAGAIN && !sets[0]);
  g_aidk_eye_started=true; schedule_mode=1;
  assert(bk7258_aidk_eye_power(false)==0 && !levels[0] && !levels[1]);
  assert(bk7258_aidk_eye_power(false)==0); /* Preserve original levels. */
  assert(bk7258_aidk_eye_power(true)==0 && levels[0]==40 && levels[1]==60);
  set_error[0]=-EIO;
  assert(bk7258_aidk_eye_power(false)==-EIO && !levels[0] && !levels[1]);
  set_error[0]=0;
  assert(bk7258_aidk_eye_power(true)==0 && levels[0]==40 && levels[1]==60);
  get_error[0]=1;
  assert(bk7258_aidk_eye_power(false)==-EIO && levels[0]==40 && !levels[1]);
  get_error[0]=0;
  assert(bk7258_aidk_eye_power(true)==0 && levels[0]==40 && levels[1]==60);
  schedule_mode=0;
  assert(bk7258_aidk_eye_power(false)==-ETIMEDOUT);
  unsigned int previous=g_eye_power_ack_sequence;
  int before=sets[0]; clock_t started=ticks; schedule_mode=2;
  assert(bk7258_aidk_eye_power(true)==0 && ticks-started==10);
  assert(g_eye_power_ack_sequence==previous+2 && sets[0]==before+2);
  assert(levels[0]==40 && levels[1]==60);
  g_aidk_lcd_panels[1].lcddev=NULL;
  assert(bk7258_aidk_eye_power(false)==-ENODEV);
  g_aidk_lcd_panels[1].lcddev=&devices[1];
  assert(bk7258_aidk_eye_power(true)==0 && levels[0]==40);
  puts("POWER_DISPLAY_PASS actual panel save/restore, partial failure, late off ack cannot satisfy resume; LCD/scheduling mocked");
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="r1-power-display-") as directory:
    path = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    path.write_text(program)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-no-pie",
                    str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

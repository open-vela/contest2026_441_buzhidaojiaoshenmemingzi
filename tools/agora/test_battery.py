#!/usr/bin/env python3
"""Run real battery conversion/status functions with GPIO/ADC/VFS stubs.

No board, ADC hardware, mailbox or audio access. This does not calibrate VBAT.
"""
import os
from pathlib import Path
import subprocess
import tempfile

PROJECT = Path(__file__).resolve().parents[2]
BOARD = PROJECT / "boards/bk7258/aidk_ai_toy"
LOWER = (BOARD / "src/bk7258_aidk_battery.c").read_text()
RPC = (PROJECT / "app/vision_badge/src/vision_badge_rpc.c").read_text()


def function(source, signature):
    # The lower half also has forward declarations: skip those.
    start = source.index(signature)
    while source.index(";", start) < source.index("{", start):
        start = source.index(signature, start + len(signature))
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


constants = "\n".join(line for line in
    (BOARD / "include/bk7258_board_config.h").read_text().splitlines()
    if line.startswith("#define BK7258_BOARD_") and any(name in line for name in
        ("PIN_5V_DET ", "PIN_FULL_DET ", "VBAT_ADC_DEV", "VBAT_SCALE_", "VBAT_OFFSET_MV")))
header = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#define FAR
#define OK 0
#define CONFIG_BK7258_AIDK_BATTERY 1
enum { BATTERY_UNKNOWN, BATTERY_FULL, BATTERY_CHARGING, BATTERY_DISCHARGING };
enum { ANIOC_RESET_FIFO=1, ANIOC_TRIGGER, BATIOC_STATE, BATIOC_GET_VOLTAGE };
struct file { int kind; };
struct __attribute__((packed)) adc_msg_s { uint8_t am_channel; int32_t am_data; };
struct battery_charger_dev_s { int unused; };
static bool present, full;
static int gpio_error, open_error, fail_ioctl, close_error, close_count, trigger_count;
static ssize_t read_size;
static struct adc_msg_s sample;
static int aidk_battery_state(struct battery_charger_dev_s *, int *);
static int aidk_battery_get_voltage(struct battery_charger_dev_s *, int *);
static int bk7258_gpio_read_input(int pin, bool *high)
{
  assert(pin==BK7258_BOARD_PIN_5V_DET || pin==BK7258_BOARD_PIN_FULL_DET);
  if (gpio_error) return gpio_error;
  *high = pin==BK7258_BOARD_PIN_5V_DET ? present : !full;
  return 0;
}
static int file_open(struct file *f, const char *path, int flags)
{
  if (open_error) return open_error;
  if (!strcmp(path, BK7258_BOARD_VBAT_ADC_DEV))
    { assert(flags & O_NONBLOCK); f->kind=1; }
  else { assert(!strcmp(path, "/dev/bat0")); f->kind=2; }
  return 0;
}
static int file_ioctl(struct file *f, int cmd, unsigned long arg)
{
  if (cmd==fail_ioctl) return -EIO;
  if (f->kind==2)
    {
      if (cmd==BATIOC_STATE) return aidk_battery_state(NULL, (int *)arg);
      assert(cmd==BATIOC_GET_VOLTAGE);
      return aidk_battery_get_voltage(NULL, (int *)arg);
    }
  assert(cmd==ANIOC_RESET_FIFO || cmd==ANIOC_TRIGGER);
  if (cmd==ANIOC_TRIGGER) trigger_count++;
  return 0;
}
static ssize_t file_read(struct file *f, char *out, size_t size)
{
  assert(f->kind==1 && size==sizeof(sample));
  if (read_size>0) memcpy(out, &sample, (size_t)read_size);
  return read_size;
}
static int file_close(struct file *f)
{ (void)f; close_count++; return close_error; }
'''
main = r'''
static void reset(void)
{
  present=false; full=false;
  gpio_error=open_error=fail_ioctl=close_error=close_count=trigger_count=0;
  sample=(struct adc_msg_s){ 0, 6000 }; read_size=sizeof(sample);
}
int main(void)
{
  char text[256]; int value;
  reset(); assert(aidk_battery_get_voltage(NULL, NULL)==-EINVAL);
  assert(aidk_battery_state(NULL, NULL)==-EINVAL);
  assert(vision_badge_battery_status(text, sizeof(text))==0);
  assert(!strcmp(text, "battery: voltage=4042 mV state=discharging"));
  assert(close_count==2 && trigger_count==1);
  reset(); present=true;
  assert(vision_badge_battery_status(text, sizeof(text))==0);
  assert(strstr(text, "state=charging"));
  full=true; assert(vision_badge_battery_status(text, sizeof(text))==0);
  assert(strstr(text, "state=full"));
  reset(); gpio_error=-EIO;
  assert(vision_badge_battery_status(text, sizeof(text))==-EIO);
  assert(strstr(text, "step=state") && trigger_count==0 && close_count==1);
  reset(); open_error=-ENOENT;
  assert(vision_badge_battery_status(text, sizeof(text))==-ENOENT);
  assert(strstr(text, "step=open") && close_count==0);
  for (int cmd=ANIOC_RESET_FIFO; cmd<=ANIOC_TRIGGER; cmd++)
    {
      reset(); fail_ioctl=cmd;
      assert(vision_badge_battery_status(text, sizeof(text))==-EIO);
      assert(strstr(text, "step=voltage") && close_count==2);
    }
  reset(); read_size=-EAGAIN;
  assert(vision_badge_battery_status(text, sizeof(text))==-EAGAIN);
  assert(close_count==2);
  reset(); read_size=sizeof(sample)-1;
  assert(aidk_battery_get_voltage(NULL, &value)==-EIO);
  reset(); sample.am_channel=1;
  assert(aidk_battery_get_voltage(NULL, &value)==-EIO);
  reset(); sample.am_data=-1;
  assert(aidk_battery_get_voltage(NULL, &value)==-EIO);
  reset(); close_error=-EBUSY;
  assert(vision_badge_battery_status(text, sizeof(text))==-EBUSY);
  reset(); read_size=-EAGAIN; close_error=-EBUSY;
  assert(aidk_battery_get_voltage(NULL, &value)==-EAGAIN);
  puts("PASS: real battery conversion/state and RPC reader, error/close/nonblocking paths; hardware stubbed");
  return 0;
}
'''
program = header.replace("enum { BATTERY_UNKNOWN", constants + "\nenum { BATTERY_UNKNOWN")
program += "\n" + "\n".join(function(LOWER, "static int " + name + "(") for name in
    ("aidk_battery_5v_present", "aidk_battery_full", "aidk_battery_state", "aidk_battery_get_voltage"))
program += "\n" + function(RPC, "static int vision_badge_battery_status(") + main
with tempfile.TemporaryDirectory(prefix="r1-battery-") as directory:
    source, binary = Path(directory) / "test.c", Path(directory) / "test"
    source.write_text(program)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-no-pie",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    # A different AP profile must fail explicitly rather than claim support.
    source.write_text("#include <errno.h>\n#include <stddef.h>\n#include <stdio.h>\n#include <assert.h>\n" +
                      function(RPC, "static int vision_badge_battery_status(") +
                      '\nint main(void) { char text[256]; assert(vision_badge_battery_status(text, sizeof(text))==-ENOSYS); return 0; }\n')
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    print("PASS: disabled battery profile reports ENOSYS")

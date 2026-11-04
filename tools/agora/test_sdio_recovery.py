#!/usr/bin/env python3
"""Exercise real SDIO command/error cleanup, with card/SDK/IRQ-only mocks.

No writes to a real card, no vendor library changes and no board audio.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "chips/bk7258/ap/bk7258_sdio.c").read_text()


def function(name):
    match = re.search(r"static\s+(?:int|bk_err_t|void)\s+" + name +
                      r"\([^;{]*\)\s*\{", source)
    assert match, name
    depth = 1
    end = source.index("{", match.start()) + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


header = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <syslog.h>
#define FAR
#define OK 0
#define BK_OK 0
#define BK_ERR_SDIO_HOST_CMD_RSP_TIMEOUT -14594
#define BK_ERR_SDIO_HOST_DATA_TIMEOUT -14595
#define BK_ERR_SDIO_HOST_NOT_INIT -14596
#define MSEC2TICK(x) (x)
#define BK7258_SDIO_CMD_WAIT_MS 100u
#define SDIO_NUTTX_RSP_MASK (15u<<6)
#define SDIO_NUTTX_RSP_SHIFT 6
#define MMCSD_CMDIDX_MASK 63u
#define MMCSD_WRXFR (4u<<10)
#define MMCSD_DATAXFR_MASK (7u<<10)
#define SDIOWAIT_ERROR 1u
#define SDIOWAIT_CMDDONE 2u
#define SDIOWAIT_RESPONSEDONE 4u
#define SDIOWAIT_TRANSFERDONE 8u
#define SDIO_HOST_CMD_RSP_NONE 0
#define SDIO_HOST_CMD_RSP_SHORT 1
#define SDIO_HOST_CMD_RSP_LONG 2
#define SDIO_HOST_RSP0 0
#define BKSDIO_TRACE(...) ((void)0)
typedef int bk_err_t;
struct sdio_dev_s { int unused; };
struct bk7258_sdio_priv_s {
  struct sdio_dev_s dev;
  bool initialized, single_write, single_read, xfer_pending, xfer_is_read;
  uint32_t cmd_timeout, events, command_r1;
  size_t blocklen, nblocks;
  int xfer_result;
};
typedef struct {
  uint32_t cmd_index, argument, wait_rsp_timeout;
  bool crc_check;
  int response;
} sdio_host_cmd_cfg_t;
static clock_t tick;
static unsigned int sent[32], count, resets, stop_count;
static bool card_receiving, lose_response, lose_stop, reject_start;
static unsigned int current;
static clock_t clock_systime_ticks(void) { return tick; }
static void nxsig_usleep(unsigned int us) { (void)us;tick++; }
static void bk_sdio_host_reset_sd_state(void) { resets++; }
static void bk_sdio_clk_gate_config(int on) { (void)on; }
static void bk_sdio_tx_fifo_clk_gate_config(int on) { (void)on; }
static void bk7258_sdio_select_single_block(void) { }
static int bk7258_sdio_complete_read(struct bk7258_sdio_priv_s *p, uint32_t cmd)
{ (void)p;(void)cmd;return 0; }
static bk_err_t bk_sdio_host_send_command(const sdio_host_cmd_cfg_t *command) {
  current=command->cmd_index;
  assert(count<32);sent[count++]=current;
  if(reject_start) return BK_ERR_SDIO_HOST_NOT_INIT;
  if(current==24u || current==25u) {
    /* A host response may be lost AFTER the card accepted the write. */
    card_receiving=true;
  } else if(current==12u) {
    stop_count++;
    if(!lose_stop) card_receiving=false;
  }
  return BK_OK;
}
static bk_err_t bk_sdio_host_wait_cmd_response(uint32_t command) {
  assert(command==current);tick+=10;
  return ((lose_response && command!=12u) || (lose_stop && command==12u)) ?
    BK_ERR_SDIO_HOST_CMD_RSP_TIMEOUT : BK_OK;
}
static uint32_t bk_sdio_host_get_cmd_rsp_argument(int index)
{ (void)index; return 0x900u; }
'''

main = r'''
static void reset(struct bk7258_sdio_priv_s *p) {
  memset(p,0,sizeof(*p));p->initialized=true;p->cmd_timeout=20000;
  p->blocklen=512;p->nblocks=1;
  tick=count=resets=stop_count=0;
  card_receiving=lose_response=lose_stop=reject_start=false;
}
int main(void) {
  struct bk7258_sdio_priv_s p;
  const uint32_t write=24u | (1u<<10) | MMCSD_WRXFR | (1u<<6);
  const uint32_t multi=25u | (1u<<10) | MMCSD_WRXFR | (1u<<6) | (1u<<13);
  reset(&p);lose_response=true;
  assert(bk7258_sdio_sendcmd(&p.dev,write,100)==-ETIMEDOUT);
  assert(count==2 && sent[1]==12u && stop_count==1);
  assert(!card_receiving && !p.single_write && !p.single_read && !p.xfer_pending);
  assert(p.xfer_result==-ETIMEDOUT && p.events==SDIOWAIT_ERROR);
  /* The next request can proceed without replaying the failed data write. */
  lose_response=false;
  assert(bk7258_sdio_sendcmd(&p.dev,write,101)==0);
  assert(p.single_write && card_receiving);
  assert(bk7258_sdio_finish_single_transfer(&p,-EIO)==-EIO);
  assert(!card_receiving);
  reset(&p);lose_response=true;
  assert(bk7258_sdio_sendcmd(&p.dev,multi,100)==-ETIMEDOUT);
  assert(count==2 && sent[1]==12u && !card_receiving);
  reset(&p);lose_response=lose_stop=true;
  assert(bk7258_sdio_sendcmd(&p.dev,write,100)==-ETIMEDOUT);
  assert(count==2 && stop_count==1 && tick<=220);
  assert(p.xfer_result==-ETIMEDOUT && p.events==SDIOWAIT_ERROR);
  /* A rejected SDK submission never reached the card: no speculative stop. */
  reset(&p);reject_start=true;
  assert(bk7258_sdio_sendcmd(&p.dev,write,100)==-EAGAIN);
  assert(count==1 && !stop_count && !card_receiving);
  reset(&p);lose_response=true;
  assert(bk7258_sdio_sendcmd(&p.dev,7u|(1u<<6),0)==-ETIMEDOUT);
  assert(count==1 && !stop_count); /* Non-data commands unchanged. */
  reset(&p);
  assert(bk7258_sdio_sendcmd(&p.dev,write,100)==0);
  assert(count==1 && !stop_count && p.single_write);
  puts("PASS: real SDIO response-error path aborts uncertain writes once, preserves error, bounds stop failure; normal/submission/non-data unchanged; SDK/card mocked");
  return 0;
}
'''

program = header + "\n".join(function(name) for name in (
    "bk7258_sdio_map_err", "bk7258_sdio_wait_command", "bk7258_sdio_retry_status",
    "bk7258_sdio_finish_stop_transmission", "bk7258_sdio_finish_single_transfer",
    "bk7258_sdio_sendcmd")) + main
with tempfile.TemporaryDirectory(prefix="r1-sdio-recovery-") as directory:
    path, binary = Path(directory) / "test.c", Path(directory) / "test"
    path.write_text(program)
    for defines in ([], ["-DCONFIG_SDIO_GDMA_EN=1"]):
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-DCONFIG_SDIO_V2P0=1", *defines, "-g", "-O1", "-no-pie",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        str(path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)

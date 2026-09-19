/****************************************************************************
 * boards/bk7258/aidk_ai_toy/src/bk7258_aidk_eye.c
 *
 * Vendor R1 dual-eye animation and product activity feedback.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_BK7258_AIDK_DUAL_LCD

#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/clock.h>
#include <nuttx/kthread.h>
#include <nuttx/kmalloc.h>
#include <nuttx/semaphore.h>

#include <arch/board/board.h>

#include "bk7258_aidk_eye_assets.h"

#define AIDK_EYE_STACKSIZE       4096
#define AIDK_EYE_PRIORITY        100
#define AIDK_EYE_PANEL_PIXELS    BK7258_BOARD_LCD_WIDTH
#define AIDK_EYE_FRAME_ROWS      BK7258_BOARD_LCD_HEIGHT
#define AIDK_EYE_OPEN_MS         2600
#define AIDK_EYE_HALF_MS         90
#define AIDK_EYE_CLOSED_MS       110
#define AIDK_EYE_DONE_MS         900
#define AIDK_EYE_ERROR_MS        1600
#define AIDK_EYE_STATUS_ROWS     8

extern int bk7258_aidk_dual_lcd_pair_putrun(unsigned int row,
                                             FAR const uint16_t *left,
                                             FAR const uint16_t *right,
                                             size_t npixels);

static sem_t g_aidk_eye_wake = SEM_INITIALIZER(0);
static volatile enum bk7258_aidk_eye_state_e g_aidk_eye_state =
  BK7258_AIDK_EYE_IDLE;
static bool g_aidk_eye_started;

struct aidk_eye_frame_s
{
  FAR const uint16_t *palette;
  FAR uint8_t *pixels;
};

static struct aidk_eye_frame_s g_aidk_eye_closed;
static struct aidk_eye_frame_s g_aidk_eye_half;
static struct aidk_eye_frame_s g_aidk_eye_open;

static int aidk_eye_base64_value(char value)
{
  if (value >= 'A' && value <= 'Z')
    {
      return value - 'A';
    }

  if (value >= 'a' && value <= 'z')
    {
      return value - 'a' + 26;
    }

  if (value >= '0' && value <= '9')
    {
      return value - '0' + 52;
    }

  if (value == '+')
    {
      return 62;
    }

  if (value == '/')
    {
      return 63;
    }

  return -EINVAL;
}

static int aidk_eye_unpack(
  FAR const struct bk7258_aidk_eye_packed_asset_s *packed,
  FAR struct aidk_eye_frame_s *frame)
{
  const size_t expected = BK7258_AIDK_EYE_ASSET_WIDTH *
                          BK7258_AIDK_EYE_ASSET_HEIGHT;
  uint32_t accumulator = 0;
  unsigned int bits = 0;
  size_t written = 0;
  size_t index;

  frame->pixels = kmm_malloc(expected);
  if (frame->pixels == NULL)
    {
      return -ENOMEM;
    }

  frame->palette = packed->palette;
  for (index = 0; index < packed->encoded_size; index++)
    {
      int value;

      if (packed->encoded_pixels[index] == '=')
        {
          break;
        }

      value = aidk_eye_base64_value(packed->encoded_pixels[index]);
      if (value < 0)
        {
          goto fail;
        }

      accumulator = (accumulator << 6) | (uint32_t)value;
      bits += 6;
      if (bits >= 8)
        {
          bits -= 8;
          if (written >= expected)
            {
              goto fail;
            }

          frame->pixels[written++] =
            (uint8_t)((accumulator >> bits) & 0xffu);
          accumulator = bits == 0 ? 0 :
            accumulator & ((1u << bits) - 1u);
        }
    }

  if (written != expected)
    {
      goto fail;
    }

  return OK;

fail:
  kmm_free(frame->pixels);
  frame->pixels = NULL;
  frame->palette = NULL;
  return -EINVAL;
}

static void aidk_eye_release_assets(void)
{
  kmm_free(g_aidk_eye_closed.pixels);
  kmm_free(g_aidk_eye_half.pixels);
  kmm_free(g_aidk_eye_open.pixels);
  memset(&g_aidk_eye_closed, 0, sizeof(g_aidk_eye_closed));
  memset(&g_aidk_eye_half, 0, sizeof(g_aidk_eye_half));
  memset(&g_aidk_eye_open, 0, sizeof(g_aidk_eye_open));
}

static int aidk_eye_load_assets(void)
{
  int ret;

  ret = aidk_eye_unpack(&g_bk7258_aidk_eye_closed_packed,
                        &g_aidk_eye_closed);
  if (ret < 0)
    {
      return ret;
    }

  ret = aidk_eye_unpack(&g_bk7258_aidk_eye_half_packed,
                        &g_aidk_eye_half);
  if (ret < 0)
    {
      aidk_eye_release_assets();
      return ret;
    }

  ret = aidk_eye_unpack(&g_bk7258_aidk_eye_open_packed,
                        &g_aidk_eye_open);
  if (ret < 0)
    {
      aidk_eye_release_assets();
      return ret;
    }

  return OK;
}

static uint16_t aidk_eye_state_colour(enum bk7258_aidk_eye_state_e state)
{
  switch (state)
    {
      case BK7258_AIDK_EYE_CAPTURE:
        return 0xf800u;
      case BK7258_AIDK_EYE_QUERY:
        return 0x001fu;
      case BK7258_AIDK_EYE_DONE:
        return 0x07e0u;
      case BK7258_AIDK_EYE_ERROR:
        return 0xf81fu;
      case BK7258_AIDK_EYE_IDLE:
      default:
        return 0;
    }
}

static int aidk_eye_render(
  FAR const struct aidk_eye_frame_s *asset,
  enum bk7258_aidk_eye_state_e state)
{
  uint16_t fb0[AIDK_EYE_PANEL_PIXELS];
  uint16_t fb1[AIDK_EYE_PANEL_PIXELS];
  uint16_t indicator = aidk_eye_state_colour(state);
  unsigned int row;
  unsigned int column;
  int ret;

  if (asset == NULL || asset->palette == NULL || asset->pixels == NULL)
    {
      return -EINVAL;
    }

  for (row = 0; row < AIDK_EYE_FRAME_ROWS; row++)
    {
      FAR const uint8_t *source =
        asset->pixels + row * BK7258_AIDK_EYE_ASSET_WIDTH;

      for (column = 0; column < AIDK_EYE_PANEL_PIXELS; column++)
        {
          /* The vendor artwork stores the physical left eye first.  R1
           * routes fb0 to the physical right display and fb1 to the left. */

          fb0[column] = asset->palette[source[AIDK_EYE_PANEL_PIXELS +
                                               column]];
          fb1[column] = asset->palette[source[column]];
        }

      if (indicator != 0 &&
          row >= AIDK_EYE_FRAME_ROWS - AIDK_EYE_STATUS_ROWS)
        {
          for (column = 0; column < AIDK_EYE_PANEL_PIXELS; column++)
            {
              fb0[column] = indicator;
              fb1[column] = indicator;
            }
        }

      ret = bk7258_aidk_dual_lcd_pair_putrun(row, fb0, fb1,
                                              AIDK_EYE_PANEL_PIXELS);
      if (ret < 0)
        {
          return ret;
        }
    }

  return OK;
}

static bool aidk_eye_wait_while(enum bk7258_aidk_eye_state_e expected,
                                unsigned int milliseconds)
{
  (void)nxsem_tickwait_uninterruptible(&g_aidk_eye_wake,
                                        MSEC2TICK(milliseconds));
  return __atomic_load_n(&g_aidk_eye_state, __ATOMIC_ACQUIRE) == expected;
}

static int aidk_eye_worker(int argc, FAR char *argv[])
{
  enum bk7258_aidk_eye_state_e state;
  int ret;

  (void)argc;
  (void)argv;

  syslog(LOG_INFO, "AIDK EYE START vendor dual-eye animation\n");
  for (;;)
    {
      state = __atomic_load_n(&g_aidk_eye_state, __ATOMIC_ACQUIRE);
      switch (state)
        {
          case BK7258_AIDK_EYE_IDLE:
            ret = aidk_eye_render(&g_aidk_eye_open, state);
            if (ret < 0)
              {
                return ret;
              }

            if (!aidk_eye_wait_while(state, AIDK_EYE_OPEN_MS))
              {
                break;
              }

            (void)aidk_eye_render(&g_aidk_eye_half, state);
            if (!aidk_eye_wait_while(state, AIDK_EYE_HALF_MS))
              {
                break;
              }

            (void)aidk_eye_render(&g_aidk_eye_closed, state);
            if (!aidk_eye_wait_while(state, AIDK_EYE_CLOSED_MS))
              {
                break;
              }

            (void)aidk_eye_render(&g_aidk_eye_half, state);
            (void)aidk_eye_wait_while(state, AIDK_EYE_HALF_MS);
            break;

          case BK7258_AIDK_EYE_CAPTURE:
            (void)aidk_eye_render(&g_aidk_eye_closed, state);
            (void)nxsem_wait_uninterruptible(&g_aidk_eye_wake);
            break;

          case BK7258_AIDK_EYE_QUERY:
            (void)aidk_eye_render(&g_aidk_eye_half, state);
            (void)nxsem_wait_uninterruptible(&g_aidk_eye_wake);
            break;

          case BK7258_AIDK_EYE_DONE:
            (void)aidk_eye_render(&g_aidk_eye_open, state);
            if (aidk_eye_wait_while(state, AIDK_EYE_DONE_MS))
              {
                __atomic_store_n(&g_aidk_eye_state,
                                 BK7258_AIDK_EYE_IDLE,
                                 __ATOMIC_RELEASE);
              }
            break;

          case BK7258_AIDK_EYE_ERROR:
          default:
            (void)aidk_eye_render(&g_aidk_eye_closed,
                                  BK7258_AIDK_EYE_ERROR);
            if (aidk_eye_wait_while(state, AIDK_EYE_ERROR_MS))
              {
                __atomic_store_n(&g_aidk_eye_state,
                                 BK7258_AIDK_EYE_IDLE,
                                 __ATOMIC_RELEASE);
              }
            break;
        }
    }
}

int bk7258_aidk_eye_set_state(enum bk7258_aidk_eye_state_e state)
{
  enum bk7258_aidk_eye_state_e previous;

  if ((unsigned int)state > BK7258_AIDK_EYE_ERROR)
    {
      return -EINVAL;
    }

  if (!g_aidk_eye_started)
    {
      return -EAGAIN;
    }

  previous = __atomic_exchange_n(&g_aidk_eye_state, state,
                                  __ATOMIC_ACQ_REL);
  if (previous == state)
    {
      return OK;
    }

  return nxsem_post(&g_aidk_eye_wake);
}

int bk7258_aidk_eye_initialize(void)
{
  cpu_set_t cpuset;
  int pid;
  int ret;

  if (g_aidk_eye_started)
    {
      return OK;
    }

  ret = aidk_eye_load_assets();
  if (ret < 0)
    {
      return ret;
    }

  pid = kthread_create("aidk-eye", AIDK_EYE_PRIORITY, AIDK_EYE_STACKSIZE,
                       aidk_eye_worker, NULL);
  if (pid < 0)
    {
      aidk_eye_release_assets();
      return pid;
    }

#ifdef CONFIG_SMP
  CPU_ZERO(&cpuset);
  CPU_SET(0, &cpuset);
  ret = sched_setaffinity(pid, sizeof(cpuset), &cpuset);
  if (ret < 0)
    {
      kthread_delete(pid);
      aidk_eye_release_assets();
      return ret;
    }
#else
  (void)cpuset;
  ret = 0;
#endif

  g_aidk_eye_started = true;
  syslog(LOG_INFO, "AIDK EYE SCHEDULED pid=%d source=vendor-genie-eye\n",
         pid);
  return OK;
}

#endif /* CONFIG_BK7258_AIDK_DUAL_LCD */

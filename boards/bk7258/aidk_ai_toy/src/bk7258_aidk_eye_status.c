/* SPDX-License-Identifier: Apache-2.0 */
/* LVGL from the locked OpenVela apps repository; RGB565 status overlay.
 * The existing eye worker owns LVGL and both panels. No new DMA/task owner. */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <lvgl/lvgl.h>
#include "bk7258_aidk_eye_status.h"

static uint16_t g_pixels[AIDK_STATUS_WIDTH * AIDK_STATUS_HEIGHT]
  __attribute__((aligned(4)));
static lv_display_t *g_display;
static lv_obj_t *g_label;
static int g_state = -1;
static bool g_flushed;

static void status_flush(lv_display_t *display, const lv_area_t *area,
                         uint8_t *pixels)
{
  (void)area;
  (void)pixels;
  /* Full RGB565 rendering writes directly to g_pixels, without panel I/O. */
  g_flushed = true;
  lv_display_flush_ready(display);
}

int bk7258_aidk_eye_status_render(unsigned int state)
{
  static const char *const labels[] = {"READY", "CAPTURE", "QUERY", "DONE", "ERROR"};
  static const uint32_t colours[] = {0xffffff, 0xff6040, 0x6090ff, 0x40ff80, 0xff60c0};
  if (state >= sizeof(labels) / sizeof(labels[0])) return -EINVAL;
  if (g_display == NULL)
    {
      lv_init();
      g_display = lv_display_create(AIDK_STATUS_WIDTH, AIDK_STATUS_HEIGHT);
      if (g_display == NULL) return -ENOMEM;
      lv_display_set_color_format(g_display, LV_COLOR_FORMAT_RGB565);
      lv_display_set_buffers(g_display, g_pixels, NULL, sizeof(g_pixels), LV_DISPLAY_RENDER_MODE_FULL);
      lv_display_set_flush_cb(g_display, status_flush);
      lv_obj_t *screen = lv_display_get_screen_active(g_display);
      lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
      lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(screen, 0, 0);
      lv_obj_set_style_pad_all(screen, 0, 0);
      g_label = lv_label_create(screen);
      if (g_label == NULL)
        { lv_display_delete(g_display); g_display = NULL; return -ENOMEM; }
    }
  if ((int)state != g_state)
    {
      lv_label_set_text_static(g_label, labels[state]);
      lv_obj_set_style_text_color(g_label, lv_color_hex(colours[state]), 0);
      lv_obj_center(g_label);
      g_flushed = false;
      lv_refr_now(g_display);
      if (!g_flushed) return -EIO;
      g_state = (int)state;
    }
  return 0;
}

const uint16_t *bk7258_aidk_eye_status_row(unsigned int row)
{
  if (g_state < 0 || row >= AIDK_STATUS_HEIGHT) return NULL;
  return g_pixels + row * AIDK_STATUS_WIDTH;
}

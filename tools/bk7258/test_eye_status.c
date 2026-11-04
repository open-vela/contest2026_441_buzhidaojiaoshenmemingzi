/* SPDX-License-Identifier: Apache-2.0 */
/* Real LVGL software rendering, no panel/DMA stubs or board success claims. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <lvgl/lvgl.h>
#include "bk7258_aidk_eye_status.h"

static uint32_t frame_hash(void)
{
  uint32_t hash = 2166136261u;
  unsigned int lit = 0;
  for (unsigned int y = 0; y < AIDK_STATUS_HEIGHT; y++)
    {
      const uint16_t *row = bk7258_aidk_eye_status_row(y);
      assert(row != NULL);
      for (unsigned int x = 0; x < AIDK_STATUS_WIDTH; x++)
        {
          lit += row[x] != 0;
          hash = (hash ^ row[x]) * 16777619u;
        }
    }
  assert(lit > 60 && lit < AIDK_STATUS_WIDTH * AIDK_STATUS_HEIGHT / 2);
  return hash;
}

int main(int argc, char **argv)
{
  uint32_t expected[5];
  lv_mem_monitor_t before;
  lv_mem_monitor_t after;
  assert(bk7258_aidk_eye_status_row(0) == NULL);
  assert(bk7258_aidk_eye_status_render(5) == -EINVAL);
  assert(bk7258_aidk_eye_status_render(UINT32_MAX) == -EINVAL);
  for (unsigned int state = 0; state < 5; state++)
    {
      assert(bk7258_aidk_eye_status_render(state) == 0);
      expected[state] = frame_hash();
      for (unsigned int previous = 0; previous < state; previous++)
        assert(expected[state] != expected[previous]);
      assert(bk7258_aidk_eye_status_row(AIDK_STATUS_HEIGHT) == NULL);
      if (argc == 2)
        {
          char path[1024];
          assert(snprintf(path, sizeof(path), "%s/state-%u.rgb565", argv[1], state) < (int)sizeof(path));
          FILE *file = fopen(path, "wb");
          assert(file != NULL);
          for (unsigned int y = 0; y < AIDK_STATUS_HEIGHT; y++)
            assert(fwrite(bk7258_aidk_eye_status_row(y), 2, AIDK_STATUS_WIDTH, file) == AIDK_STATUS_WIDTH);
          assert(fclose(file) == 0);
        }
    }
  /* Re-enter every state repeatedly: catch stale frames and allocator growth. */
  lv_mem_monitor(&before);
  for (unsigned int cycle = 0; cycle < 100; cycle++)
    for (int state = 4; state >= 0; state--)
      {
        assert(bk7258_aidk_eye_status_render((unsigned int)state) == 0);
        assert(frame_hash() == expected[state]);
        assert(bk7258_aidk_eye_status_render((unsigned int)state) == 0);
        assert(frame_hash() == expected[state]);
      }
  lv_mem_monitor(&after);
  assert(before.free_size == after.free_size);
  printf("LVGL_POOL total=%zu free=%zu max_used=%zu; host allocator sizes\n",
         after.total_size, after.free_size, after.max_used);
  puts("REAL_LVGL_RGB565_RENDER_PASS 5 states, 500 transitions, no pool growth; hardware unverified");
  return 0;
}

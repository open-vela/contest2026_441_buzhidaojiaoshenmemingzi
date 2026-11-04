/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <nuttx/mutex.h>
#include <vision_badge/xiaozhi_audio.h>

struct xiaozhi_playout_s
{
  mutex_t lock;
  uint32_t epoch;
  bool active;
  bool playing;
  unsigned int head;
  struct xiaozhi_playout_stats_s stats;
  int16_t pcm[XIAOZHI_PLAYOUT_FRAMES][XIAOZHI_FRAME_SAMPLES];
};

int xiaozhi_playout_open(struct xiaozhi_playout_s **result)
{
  struct xiaozhi_playout_s *queue;
  int ret;
  if (result == NULL)
    {
      return -EINVAL;
    }

  *result = NULL;
  queue = calloc(1, sizeof(*queue));
  if (queue == NULL)
    {
      return -ENOMEM;
    }

  ret = nxmutex_init(&queue->lock);
  if (ret < 0)
    {
      free(queue);
      return ret;
    }

  *result = queue;
  return 0;
}

void xiaozhi_playout_close(struct xiaozhi_playout_s *queue)
{
  /* Caller joins producer and consumer before destroying the queue. */
  if (queue != NULL)
    {
      nxmutex_destroy(&queue->lock);
      free(queue);
    }
}

uint32_t xiaozhi_playout_begin(struct xiaozhi_playout_s *queue)
{
  uint32_t epoch;
  if (queue == NULL)
    {
      return 0;
    }

  nxmutex_lock(&queue->lock);
  if (++queue->epoch == 0)
    {
      ++queue->epoch;
    }

  queue->active = true;
  queue->playing = false;
  queue->head = 0;
  memset(&queue->stats, 0, sizeof(queue->stats));
  epoch = queue->epoch;
  nxmutex_unlock(&queue->lock);
  return epoch;
}

static int valid_epoch(struct xiaozhi_playout_s *queue, uint32_t epoch)
{
  return queue->active && epoch != 0 && epoch == queue->epoch ? 0 : -ESTALE;
}

int xiaozhi_playout_push(struct xiaozhi_playout_s *queue, uint32_t epoch,
                         const int16_t *pcm, size_t samples)
{
  unsigned int tail;
  int ret;
  if (queue == NULL || pcm == NULL || samples != XIAOZHI_FRAME_SAMPLES)
    {
      return -EINVAL;
    }

  nxmutex_lock(&queue->lock);
  ret = valid_epoch(queue, epoch);
  if (ret == 0)
    {
      if (queue->stats.finished)
        {
          ret = -EPIPE;
        }
      else if (queue->stats.queued == XIAOZHI_PLAYOUT_FRAMES)
        {
          ret = -EAGAIN;
        }
      else
        {
          tail = (queue->head + queue->stats.queued) % XIAOZHI_PLAYOUT_FRAMES;
          memcpy(queue->pcm[tail], pcm, sizeof(queue->pcm[tail]));
          queue->stats.queued++;
          queue->stats.accepted++;
        }
    }

  nxmutex_unlock(&queue->lock);
  return ret;
}

int xiaozhi_playout_finish(struct xiaozhi_playout_s *queue, uint32_t epoch)
{
  int ret;
  if (queue == NULL)
    {
      return -EINVAL;
    }

  nxmutex_lock(&queue->lock);
  ret = valid_epoch(queue, epoch);
  if (ret == 0)
    {
      queue->stats.finished = true;
    }

  nxmutex_unlock(&queue->lock);
  return ret;
}

int xiaozhi_playout_pop(struct xiaozhi_playout_s *queue, uint32_t epoch,
                        int16_t *pcm, size_t capacity)
{
  int ret;
  if (queue == NULL || pcm == NULL || capacity < XIAOZHI_FRAME_SAMPLES)
    {
      return -EINVAL;
    }

  nxmutex_lock(&queue->lock);
  ret = valid_epoch(queue, epoch);
  if (ret == 0)
    {
      if (queue->stats.finished && queue->stats.queued == 0)
        {
          queue->playing = false;
          ret = 0;
        }
      else if (queue->stats.queued == 0)
        {
          if (queue->playing)
            {
              queue->stats.underruns++;
              queue->playing = false;
            }

          ret = -EAGAIN;
        }
      else if (!queue->playing && !queue->stats.finished &&
               queue->stats.queued < XIAOZHI_PREBUFFER)
        {
          ret = -EAGAIN;
        }
      else
        {
          queue->playing = true;
          memcpy(pcm, queue->pcm[queue->head], sizeof(queue->pcm[0]));
          queue->head = (queue->head + 1) % XIAOZHI_PLAYOUT_FRAMES;
          queue->stats.queued--;
          queue->stats.consumed++;
          ret = XIAOZHI_FRAME_SAMPLES;
        }
    }

  nxmutex_unlock(&queue->lock);
  return ret;
}

int xiaozhi_playout_stats(struct xiaozhi_playout_s *queue, uint32_t epoch,
                          struct xiaozhi_playout_stats_s *stats)
{
  int ret;
  if (queue == NULL || stats == NULL)
    {
      return -EINVAL;
    }

  nxmutex_lock(&queue->lock);
  ret = valid_epoch(queue, epoch);
  if (ret == 0)
    {
      *stats = queue->stats;
    }

  nxmutex_unlock(&queue->lock);
  return ret;
}

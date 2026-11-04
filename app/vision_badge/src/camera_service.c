/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <syslog.h>

#ifdef __NuttX__
#  include <poll.h>
#  include <sys/ioctl.h>
#  include <sys/mman.h>
#  include <sys/videoio.h>
#endif

#include <vision_badge/config.h>
#include <vision_badge/services.h>

#if defined(__NuttX__) && defined(CONFIG_BK7258_AIDK_CAMERA) && \
    defined(CONFIG_BK7258_AP_CORE)
#  define CAMERA_WARM_ENABLED 1
#  include <stdlib.h>
#  include <nuttx/kthread.h>
#  include <nuttx/mutex.h>
#  include <nuttx/semaphore.h>
#  include <nuttx/signal.h>
static int camera_warm_capture(struct vision_badge_image_s *image);
static int camera_warm_pair(struct vision_badge_image_s *early,
                            struct vision_badge_image_s *settled);
#endif

#ifndef O_NONBLOCK
#  define O_NONBLOCK 0
#endif

#ifdef __NuttX__
#  define VISION_BADGE_CAMERA_WIDTH       640
#  define VISION_BADGE_CAMERA_HEIGHT      480
#  define VISION_BADGE_CAMERA_BUFFERS     2
#  define VISION_BADGE_CAMERA_TIMEOUT_MS  5000
#  define VISION_BADGE_CAMERA_SETTLE_MS   1000
#  define VISION_BADGE_CAMERA_PAIR_MS     2000

static uint64_t camera_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + ts.tv_nsec / 1000000u;
}

static bool camera_service_jpeg_valid(const uint8_t *data, size_t length)
{
  bool have_sos = false;
  size_t offset;

  if (length < 4 || data[0] != 0xff || data[1] != 0xd8)
    {
      return false;
    }

  offset = 2;
  while (offset + 1 < length)
    {
      uint8_t marker;
      size_t segment_length;

      if (data[offset] != 0xff)
        {
          break;
        }

      while (offset + 1 < length && data[offset + 1] == 0xff)
        {
          offset++;
        }

      if (offset + 1 >= length)
        {
          break;
        }

      marker = data[offset + 1];
      if (marker == 0xda)
        {
          have_sos = true;
          break;
        }

      if (marker == 0xd9 || offset + 3 >= length)
        {
          break;
        }

      segment_length = ((size_t)data[offset + 2] << 8) |
                       data[offset + 3];
      if (segment_length < 2 || segment_length > length - offset - 2)
        {
          break;
        }

      offset += 2 + segment_length;
    }

  return have_sos && data[length - 2] == 0xff &&
         data[length - 1] == 0xd9;
}
#endif

int camera_service_probe(struct vision_badge_probe_s *probe)
{
  int fd;

  if (probe == NULL)
    {
      return -EINVAL;
    }

  fd = open(CONFIG_CONTEST2026_441_VISION_BADGE_CAMERA_DEVPATH,
            O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      probe->available = false;
      probe->error_code = errno;
      return -errno;
    }

  close(fd);
  probe->available = true;
  probe->error_code = 0;
  return 0;
}

/* ========================================================================
 * camera_service_capture —— 通过 V4L2 采集一帧 640×480 JPEG 图片
 *
 * 完整流程：
 *   1. 打开 /dev/video0（GC2145 摄像头设备）
 *   2. 设置采集格式为 640×480 MJPEG（V4L2_PIX_FMT_JPEG）
 *   3. 申请 2 个 mmap 缓冲区（双缓冲，防止丢帧）
 *   4. 启动视频流（VIDIOC_STREAMON）
 *   5. 用 poll() 等待首帧（最长等 5 秒），持续取帧并归还缓冲
 *   6. 首帧后至少 1 秒选择新帧；对照模式同时保留首帧
 *   7. 验证 JPEG 头尾，拷贝到用户提供的 image->data 缓冲区
 *
 * 清理（无论成功还是失败都执行）：
 *   - 停止视频流（VIDIOC_STREAMOFF）
 *   - 通用配置 munmap；R1 flat 不调用不匹配的 munmap
 *   - VIDIOC_REQBUFS count=0（当前 NuttX capture 实现为空操作）
 *   - 关闭设备文件，R1 的最后一次 close 释放驱动缓冲
 *
 * 硬件链路：GC2145 传感器 → DVP 并行接口 → YUV/DMA → PSRAM → JPEG 编码器
 *   实际由 BK7258 驱动层完成，这里只通过 V4L2 标准接口与驱动交互。
 * ======================================================================== */
static int camera_capture(struct vision_badge_image_s *image,
                           struct vision_badge_image_s *settled)
{
  if (image == NULL || image->data == NULL || image->capacity == 0 ||
      (settled != NULL && (settled->data == NULL || settled->capacity == 0 ||
                           settled == image || settled->data == image->data)))
    {
      return -EINVAL;
    }

  image->size = 0;
  image->width = 0;
  image->height = 0;
  image->format = VISION_BADGE_IMAGE_JPEG;
  if (settled != NULL)
    {
      settled->size = 0;
      settled->width = 0;
      settled->height = 0;
      settled->format = VISION_BADGE_IMAGE_JPEG;
    }

#ifdef __NuttX__
  struct v4l2_requestbuffers req;
  struct v4l2_format fmt;
  struct v4l2_buffer buffer;
  struct pollfd pollfd;
  void *mapped[VISION_BADGE_CAMERA_BUFFERS] = {NULL};
  size_t mapped_length[VISION_BADGE_CAMERA_BUFFERS] = {0};
  bool buffers_requested = false;
  bool streaming = false;
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  int fd = -1;
  int ret = 0;
  int poll_ret;
  unsigned int i;
  unsigned int frames = 0;
  uint64_t started = camera_now_ms();
  uint64_t opened = started, configured = started, stream_started = started;
  uint64_t first = 0, selected = started, cleanup_started;
  uint64_t deadline;

  /* ---- 1. 打开摄像头设备 ---- */
  fd = open(CONFIG_CONTEST2026_441_VISION_BADGE_CAMERA_DEVPATH, O_RDWR);
  if (fd < 0)
    {
      ret = -errno;
      goto out;
    }
  opened = camera_now_ms();

  /* ---- 2. 设置采集格式：640×480 MJPEG ---- */
  memset(&fmt, 0, sizeof(fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = VISION_BADGE_CAMERA_WIDTH;
  fmt.fmt.pix.height = VISION_BADGE_CAMERA_HEIGHT;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;      /* MJPEG 压缩格式 */
  fmt.fmt.pix.field = V4L2_FIELD_ANY;
  fmt.fmt.pix.sizeimage = image->capacity;

  if (ioctl(fd, VIDIOC_S_FMT, (uintptr_t)&fmt) < 0)
    {
      ret = -errno;
      goto out;
    }

  if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_JPEG)
    {
      ret = -ENOTSUP;
      goto out;
    }

  /* ---- 3. 申请 2 个 mmap 缓冲区（双缓冲） ---- */
  memset(&req, 0, sizeof(req));
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  req.count = VISION_BADGE_CAMERA_BUFFERS;

  if (ioctl(fd, VIDIOC_REQBUFS, (uintptr_t)&req) < 0)
    {
      ret = -errno;
      goto out;
    }

  buffers_requested = true;
  if (req.count == 0 || req.count > VISION_BADGE_CAMERA_BUFFERS)
    {
      ret = -ENOMEM;
      goto out;
    }

  /* 查询每个缓冲区的物理地址，然后 mmap 到用户空间 */
  for (i = 0; i < req.count; i++)
    {
      memset(&buffer, 0, sizeof(buffer));
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.index = i;

      if (ioctl(fd, VIDIOC_QUERYBUF, (uintptr_t)&buffer) < 0)
        {
          ret = -errno;
          goto out;
        }

      mapped[i] = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, (off_t)buffer.m.offset);
      if (mapped[i] == MAP_FAILED)
        {
          mapped[i] = NULL;
          ret = -errno;
          goto out;
        }

      mapped_length[i] = buffer.length;

      /* 将空缓冲区放入驱动的采集队列 */
      if (ioctl(fd, VIDIOC_QBUF, (uintptr_t)&buffer) < 0)
        {
          ret = -errno;
          goto out;
        }
    }

  configured = camera_now_ms();
  /* ---- 4. 启动视频流 ---- */
  if (ioctl(fd, VIDIOC_STREAMON, (uintptr_t)&type) < 0)
    {
      ret = -errno;
      goto out;
    }

  streaming = true;
  stream_started = camera_now_ms();
  deadline = stream_started + VISION_BADGE_CAMERA_TIMEOUT_MS;

  /* An absolute deadline also bounds repeated EINTR. Keep consuming and
   * requeueing frames in BOTH modes: the R1 same-stream comparison showed
   * a green first frame and a neutral later frame after one second. Sleeping
   * with a full queue instead gives an OLD frame. Single capture must not
   * silently fall back to the unready first frame when settling times out.
   */
  pollfd.fd = fd;
  pollfd.events = POLLIN;
  for (;;)
    {
      uint64_t now = camera_now_ms();
      struct vision_badge_image_s *target = NULL;
      if (now >= deadline || frames >= 120)
        { ret = -ETIMEDOUT; goto out; }
      pollfd.revents = 0;
      poll_ret = poll(&pollfd, 1, (int)(deadline - now));
      if (poll_ret < 0 && errno == EINTR) continue;
      if (poll_ret <= 0)
        { ret = poll_ret == 0 ? -ETIMEDOUT : -errno; goto out; }
      if ((pollfd.revents & POLLIN) == 0)
        { ret = -EIO; goto out; }

      memset(&buffer, 0, sizeof(buffer));
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_MMAP;
      if (ioctl(fd, VIDIOC_DQBUF, (uintptr_t)&buffer) < 0)
        { ret = -errno; goto out; }
      frames++;
      now = camera_now_ms();
      if (now >= deadline)
        { ret = -ETIMEDOUT; goto out; }
      if (buffer.index >= req.count ||
          (buffer.flags & V4L2_BUF_FLAG_ERROR) != 0 || buffer.bytesused == 0 ||
          buffer.bytesused > mapped_length[buffer.index])
        { ret = -EIO; goto out; }
      if (!camera_service_jpeg_valid(mapped[buffer.index], buffer.bytesused))
        { ret = -EBADMSG; goto out; }
      if (frames == 1)
        {
          first = now;
          deadline = first + VISION_BADGE_CAMERA_PAIR_MS;
          if (settled != NULL) target = image;
        }
      else if (now - first >= VISION_BADGE_CAMERA_SETTLE_MS)
        {
          target = settled != NULL ? settled : image;
        }
      if (target != NULL)
        {
          if (buffer.bytesused > target->capacity)
            { ret = -EFBIG; goto out; }
          memcpy(target->data, mapped[buffer.index], buffer.bytesused);
          target->size = buffer.bytesused;
          target->width = fmt.fmt.pix.width;
          target->height = fmt.fmt.pix.height;
          selected = now;
          if (settled == NULL || target == settled) break;
        }
      if (ioctl(fd, VIDIOC_QBUF, (uintptr_t)&buffer) < 0)
        { ret = -errno; goto out; }
    }

out:
  cleanup_started = camera_now_ms();
  /* ---- 清理：停止流、unmap、释放缓冲、关闭设备 ---- */
  if (streaming && ioctl(fd, VIDIOC_STREAMOFF, (uintptr_t)&type) < 0 &&
      ret == 0)
    {
      ret = -errno;
    }

#if !defined(CONFIG_BK7258_AIDK_CAMERA) || !defined(CONFIG_BUILD_FLAT)
  for (i = 0; i < VISION_BADGE_CAMERA_BUFFERS; i++)
    {
      if (mapped[i] != NULL)
        {
          if (munmap(mapped[i], mapped_length[i]) < 0 && ret == 0)
            ret = -errno;
        }
    }
#endif

  /* R1's NuttX capture_mmap() returns a driver-owned address without adding
   * an mm_map entry or munmap callback. munmap() therefore returns EINVAL;
   * it is not the release operation for this flat-build capture device.
   * capture_close() -> cleanup_streamresources() frees bufheap on last close.
   * Keep normal munmap error checking for other builds/devices.
   */

  if (buffers_requested)
    {
      req.count = 0;
      if (ioctl(fd, VIDIOC_REQBUFS, (uintptr_t)&req) < 0 && ret == 0)
        ret = -errno;
    }

  if (fd >= 0 && close(fd) < 0 && ret == 0) ret = -errno;
  if (ret < 0)
    {
      image->size = 0;
      image->width = 0;
      image->height = 0;
      if (settled != NULL)
        { settled->size = 0; settled->width = 0; settled->height = 0; }
    }

  syslog(ret < 0 ? LOG_ERR : LOG_INFO,
         "vision_badge: camera timing open=%lu setup=%lu start=%lu "
         "first=%lu span=%lu cleanup=%lu total=%lu ms frames=%u ret=%d\n",
         (unsigned long)(opened - started),
         (unsigned long)(configured >= opened ? configured - opened : 0),
         (unsigned long)(stream_started >= configured ? stream_started - configured : 0),
         (unsigned long)(first ? first - stream_started : 0),
         (unsigned long)(first && selected >= first ? selected - first : 0),
         (unsigned long)(camera_now_ms() - cleanup_started),
         (unsigned long)(camera_now_ms() - started), frames, ret);

  return ret;
#else
  return -ENOSYS;
#endif
}

int camera_service_capture(struct vision_badge_image_s *image)
{
#ifdef CAMERA_WARM_ENABLED
  return camera_warm_capture(image);
#else
  return camera_capture(image, NULL);
#endif
}

int camera_service_capture_pair(struct vision_badge_image_s *early,
                                struct vision_badge_image_s *settled)
{
  if (!early || !settled || !early->data || !settled->data ||
      !early->capacity || !settled->capacity || early == settled ||
      early->data == settled->data) return -EINVAL;
#ifdef CAMERA_WARM_ENABLED
  return camera_warm_pair(early, settled);
#else
  return camera_capture(early, settled);
#endif
}

#ifdef CAMERA_WARM_ENABLED
/* One worker owns all V4L2 operations. Callers only copy a complete JPEG
 * under lock; no driver buffer or caller pointer survives a request.
 * Keep draining/requeueing while HTTP inference runs, rather than returning
 * an old queued frame at the next button press. Nothing uploads by itself.
 */
static mutex_t g_camera_api_lock = NXMUTEX_INITIALIZER;
static struct
{
  mutex_t lock;
  sem_t wake;
  bool initialized, wanted, busy, ready, blocked, paused, reserved, resume_wanted;
  int error;
  uint8_t *jpeg;
  size_t size;
  uint64_t first, updated;
  uint32_t sequence;
} g_camera = { .lock = NXMUTEX_INITIALIZER };

struct camera_stream_s
{
  int fd;
  bool buffers, streaming;
  struct v4l2_requestbuffers req;
  void *mapped[VISION_BADGE_CAMERA_BUFFERS];
  size_t lengths[VISION_BADGE_CAMERA_BUFFERS];
};

static int camera_stream_close(struct camera_stream_s *stream)
{
  int ret = 0;
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (stream->streaming && ioctl(stream->fd, VIDIOC_STREAMOFF,
                                (uintptr_t)&type) < 0) ret = -errno;
#ifndef CONFIG_BUILD_FLAT
  for (unsigned int i = 0; i < VISION_BADGE_CAMERA_BUFFERS; i++)
    if (stream->mapped[i] && munmap(stream->mapped[i], stream->lengths[i]) < 0 && ret == 0)
      ret = -errno;
#endif
  if (stream->buffers)
    {
      stream->req.count = 0;
      if (ioctl(stream->fd, VIDIOC_REQBUFS, (uintptr_t)&stream->req) < 0 && ret == 0)
        ret = -errno;
    }
  if (stream->fd >= 0 && close(stream->fd) < 0 && ret == 0) ret = -errno;
  memset(stream, 0, sizeof(*stream));
  stream->fd = -1;
  return ret;
}

static int camera_stream_open(struct camera_stream_s *stream)
{
  struct v4l2_format fmt = {0};
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  stream->fd = open(CONFIG_CONTEST2026_441_VISION_BADGE_CAMERA_DEVPATH,
                    O_RDWR | O_NONBLOCK);
  if (stream->fd < 0) return -errno;
  fmt.type = type;
  fmt.fmt.pix.width = VISION_BADGE_CAMERA_WIDTH;
  fmt.fmt.pix.height = VISION_BADGE_CAMERA_HEIGHT;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
  fmt.fmt.pix.field = V4L2_FIELD_ANY;
  fmt.fmt.pix.sizeimage = CONFIG_CONTEST2026_441_VISION_BADGE_JPEG_BUFFER_SIZE;
  if (ioctl(stream->fd, VIDIOC_S_FMT, (uintptr_t)&fmt) < 0) return -errno;
  if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_JPEG ||
      fmt.fmt.pix.width != VISION_BADGE_CAMERA_WIDTH ||
      fmt.fmt.pix.height != VISION_BADGE_CAMERA_HEIGHT) return -ENOTSUP;
  stream->req.type = type;
  stream->req.memory = V4L2_MEMORY_MMAP;
  stream->req.count = VISION_BADGE_CAMERA_BUFFERS;
  if (ioctl(stream->fd, VIDIOC_REQBUFS, (uintptr_t)&stream->req) < 0) return -errno;
  stream->buffers = true;
  if (!stream->req.count || stream->req.count > VISION_BADGE_CAMERA_BUFFERS) return -ENOMEM;
  for (unsigned int i = 0; i < stream->req.count; i++)
    {
      struct v4l2_buffer buffer = {0};
      buffer.type = type;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.index = i;
      if (ioctl(stream->fd, VIDIOC_QUERYBUF, (uintptr_t)&buffer) < 0) return -errno;
      if (!buffer.length) return -EIO;
      void *mapped = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
                          MAP_SHARED, stream->fd, (off_t)buffer.m.offset);
      if (mapped == MAP_FAILED) return -errno;
      stream->mapped[i] = mapped;
      stream->lengths[i] = buffer.length;
      if (ioctl(stream->fd, VIDIOC_QBUF, (uintptr_t)&buffer) < 0) return -errno;
    }
  if (ioctl(stream->fd, VIDIOC_STREAMON, (uintptr_t)&type) < 0) return -errno;
  stream->streaming = true;
  return 0;
}

static int camera_warm_worker(int argc, char *argv[])
{
  struct camera_stream_s stream = { .fd = -1 };
  uint64_t last_frame = 0;
  (void)argc; (void)argv;
  for (;;)
    {
      nxmutex_lock(&g_camera.lock);
      bool wanted = g_camera.wanted;
      /* Publish ownership BEFORE slow open so stop cannot race past it. */
      if (wanted) g_camera.busy = true;
      nxmutex_unlock(&g_camera.lock);
      if (!wanted)
        {
          int close_ret = camera_stream_close(&stream);
          nxmutex_lock(&g_camera.lock);
          g_camera.ready = false;
          g_camera.size = 0;
          free(g_camera.jpeg); g_camera.jpeg = NULL;
          if (close_ret < 0)
            { g_camera.error = close_ret; g_camera.blocked = true; g_camera.wanted = false; }
          g_camera.busy = false;
          nxmutex_unlock(&g_camera.lock);
          nxsem_wait_uninterruptible(&g_camera.wake);
          continue;
        }
      int ret = 0;
      if (stream.fd < 0)
        {
          ret = camera_stream_open(&stream);
          nxmutex_lock(&g_camera.lock);
          if (ret == 0)
            {
              g_camera.jpeg = malloc(CONFIG_CONTEST2026_441_VISION_BADGE_JPEG_BUFFER_SIZE);
              if (!g_camera.jpeg) ret = -ENOMEM;
            }
          g_camera.first = 0;
          g_camera.ready = false;
          last_frame = camera_now_ms();
          nxmutex_unlock(&g_camera.lock);
          syslog(ret < 0 ? LOG_ERR : LOG_INFO,
                 "vision_badge: camera warm open ret=%d\n", ret);
        }
      if (ret == 0)
        {
          struct v4l2_buffer buffer = {0};
          buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
          buffer.memory = V4L2_MEMORY_MMAP;
          /* NuttX capture_dqbuf explicitly supports O_NONBLOCK/EAGAIN.
           * Drain a queued frame before consulting the idle watchdog. A
           * poll timeout can be stale after this low-priority worker was
           * descheduled; readiness is only a hint, not a frame-loss verdict.
           * Nonblocking DQBUF also tolerates a wake before completion queues
           * its buffer. Keep the actual no-frame deadline unchanged. */
          if (ioctl(stream.fd, VIDIOC_DQBUF, (uintptr_t)&buffer) < 0)
            {
              int error = errno;
              if (error != EAGAIN && error != EINTR) ret = -error;
              else if (error == EAGAIN)
                {
                  if (camera_now_ms() - last_frame >= 2000) ret = -ETIMEDOUT;
                  else
                    {
                      struct pollfd pfd = { .fd = stream.fd, .events = POLLIN };
                      int polled = poll(&pfd, 1, 100);
                      if (polled < 0 && errno != EINTR) ret = -errno;
                      else if (polled > 0 && !(pfd.revents & POLLIN)) ret = -EIO;
                    }
                }
            }
          else if (buffer.index >= stream.req.count ||
                   (buffer.flags & V4L2_BUF_FLAG_ERROR) || !buffer.bytesused ||
                   buffer.bytesused > stream.lengths[buffer.index] ||
                   buffer.bytesused > CONFIG_CONTEST2026_441_VISION_BADGE_JPEG_BUFFER_SIZE)
            ret = -EIO;
          else if (!camera_service_jpeg_valid(stream.mapped[buffer.index], buffer.bytesused))
            ret = -EBADMSG;
          else
            {
              last_frame = camera_now_ms();
              nxmutex_lock(&g_camera.lock);
              if (!g_camera.first) g_camera.first = last_frame;
              bool publish = g_camera.wanted && last_frame - g_camera.first >= VISION_BADGE_CAMERA_SETTLE_MS;
              if (publish) memcpy(g_camera.jpeg, stream.mapped[buffer.index], buffer.bytesused);
              /* Copy before returning the buffer; publish only if return succeeds. */
              if (ioctl(stream.fd, VIDIOC_QBUF, (uintptr_t)&buffer) < 0) ret = -errno;
              else if (publish)
                {
                  bool first_ready = !g_camera.ready;
                  g_camera.size = buffer.bytesused;
                  g_camera.updated = last_frame;
                  g_camera.sequence++;
                  g_camera.ready = true;
                  if (first_ready) syslog(LOG_INFO, "vision_badge: camera warm ready\n");
                }
              nxmutex_unlock(&g_camera.lock);
            }
        }
      if (ret < 0)
        {
          nxmutex_lock(&g_camera.lock);
          g_camera.ready = false;
          nxmutex_unlock(&g_camera.lock);
          int close_ret = camera_stream_close(&stream);
          nxmutex_lock(&g_camera.lock);
          free(g_camera.jpeg); g_camera.jpeg = NULL; g_camera.size = 0;
          g_camera.error = close_ret < 0 ? close_ret : ret;
          g_camera.blocked = close_ret < 0;
          g_camera.wanted = false; g_camera.busy = false;
          nxmutex_unlock(&g_camera.lock);
          syslog(LOG_ERR, "vision_badge: camera warm stopped ret=%d\n", ret);
        }
    }
  return 0;
}

/* g_camera_api_lock serializes start/stop, snapshots and cold diagnostics. */
static int camera_warm_start_locked(void)
{
  int ret = 0;
  nxmutex_lock(&g_camera.lock);
  if (g_camera.paused || g_camera.reserved) ret = -EBUSY;
  else if (g_camera.blocked) ret = g_camera.error;
  else if (!g_camera.initialized)
    {
      ret = nxsem_init(&g_camera.wake, 0, 0);
      if (ret == 0)
        {
          ret = kthread_create("vision-camera", 98, 16384, camera_warm_worker, NULL);
          if (ret < 0) nxsem_destroy(&g_camera.wake);
          else { g_camera.initialized = true; ret = 0; }
        }
    }
  if (ret == 0 && !g_camera.wanted)
    {
      g_camera.error = 0; g_camera.ready = false;
      g_camera.wanted = true;
      nxsem_post(&g_camera.wake);
    }
  nxmutex_unlock(&g_camera.lock);
  return ret;
}

static int camera_warm_stop_locked(void)
{
  uint64_t deadline = camera_now_ms() + 3000;
  nxmutex_lock(&g_camera.lock);
  g_camera.wanted = false; g_camera.ready = false;
  nxmutex_unlock(&g_camera.lock);
  for (;;)
    {
      nxmutex_lock(&g_camera.lock);
      bool busy = g_camera.busy;
      int ret = g_camera.blocked ? g_camera.error : 0;
      nxmutex_unlock(&g_camera.lock);
      if (!busy) return ret;
      if (camera_now_ms() >= deadline) return -ETIMEDOUT;
      nxsig_usleep(20000);
    }
}

static int camera_warm_snapshot(struct vision_badge_image_s *image, bool borrowed,
                               unsigned int timeout_ms,
                               bool (*cancel)(void *), void *arg)
{
  if (image) image->size = image->width = image->height = 0;
  if (!image || !image->data || !image->capacity || !timeout_ms) return -EINVAL;
  image->format = VISION_BADGE_IMAGE_JPEG;
  nxmutex_lock(&g_camera_api_lock);
  uint64_t started = camera_now_ms();
  if (timeout_ms > VISION_BADGE_CAMERA_TIMEOUT_MS) timeout_ms = VISION_BADGE_CAMERA_TIMEOUT_MS;
  uint64_t deadline = started + timeout_ms;
  bool restarted = false;
  int ret;
  if (borrowed)
    {
      nxmutex_lock(&g_camera.lock);
      ret = !g_camera.reserved || g_camera.paused ? -EBUSY :
            g_camera.blocked ? (g_camera.error < 0 ? g_camera.error : -EIO) : 0;
      nxmutex_unlock(&g_camera.lock);
    }
  else ret = camera_warm_start_locked();
  nxmutex_lock(&g_camera.lock);
  uint32_t previous = g_camera.sequence;
  nxmutex_unlock(&g_camera.lock);
  while (ret == 0)
    {
      if (cancel && cancel(arg)) { ret = -ECANCELED; break; }
      if (camera_now_ms() >= deadline) { ret = -ETIMEDOUT; break; }
      nxmutex_lock(&g_camera.lock);
      uint64_t now = camera_now_ms();
      /* A live-session owner survives an idle frame timeout. Reuse the
       * existing worker only after its successful close released buffers.
       * Recover once on demand, retaining the original request deadline,
       * reservation and resume intent; never return the pre-timeout cache.
       * Failed close/device errors remain quarantined. */
      if (borrowed && !restarted && g_camera.initialized &&
          g_camera.reserved && !g_camera.paused && !g_camera.blocked &&
          !g_camera.wanted && !g_camera.busy &&
          g_camera.error == -ETIMEDOUT)
        {
          restarted = true;
          previous = g_camera.sequence;
          g_camera.error = 0; g_camera.ready = false;
          g_camera.wanted = true;
          nxsem_post(&g_camera.wake);
        }
      if (!g_camera.wanted) ret = g_camera.error < 0 ? g_camera.error : -ECANCELED;
      /* The two upper-half slots could contain pre-request frames if this
       * worker was delayed. Observe three newly returned frames, not just
       * the old cache, before satisfying the button request.
       */
      else if (g_camera.ready &&
               (uint32_t)(g_camera.sequence - previous) > VISION_BADGE_CAMERA_BUFFERS &&
               now - g_camera.updated <= 200)
        {
          if (g_camera.size > image->capacity) ret = -EFBIG;
          else
            {
              memcpy(image->data, g_camera.jpeg, g_camera.size);
              image->size = g_camera.size;
              image->width = VISION_BADGE_CAMERA_WIDTH;
              image->height = VISION_BADGE_CAMERA_HEIGHT;
            }
          nxmutex_unlock(&g_camera.lock);
          break;
        }
      nxmutex_unlock(&g_camera.lock);
      if (ret < 0) break;
      if (camera_now_ms() >= deadline) { ret = -ETIMEDOUT; break; }
      nxsig_usleep(10000);
    }
  nxmutex_unlock(&g_camera_api_lock);
  syslog(ret < 0 ? LOG_ERR : LOG_INFO,
         "vision_badge: camera warm snapshot total=%lu ms bytes=%lu ret=%d restarted=%d\n",
         (unsigned long)(camera_now_ms() - started), (unsigned long)image->size, ret, restarted);
  return ret;
}

static int camera_warm_capture(struct vision_badge_image_s *image)
{
  return camera_warm_snapshot(image, false, VISION_BADGE_CAMERA_TIMEOUT_MS, NULL, NULL);
}

static int camera_warm_pair(struct vision_badge_image_s *early,
                            struct vision_badge_image_s *settled)
{
  nxmutex_lock(&g_camera_api_lock);
  nxmutex_lock(&g_camera.lock);
  if (g_camera.paused || g_camera.reserved)
    {
      nxmutex_unlock(&g_camera.lock);
      nxmutex_unlock(&g_camera_api_lock);
      return -EBUSY;
    }
  bool restart = g_camera.wanted;
  nxmutex_unlock(&g_camera.lock);
  int ret = camera_warm_stop_locked();
  if (ret == 0) ret = camera_capture(early, settled);
  if (restart)
    {
      int start_ret = camera_warm_start_locked();
      if (ret == 0) ret = start_ret;
    }
  nxmutex_unlock(&g_camera_api_lock);
  return ret;
}
#endif

int camera_service_capture_paused(struct vision_badge_image_s *image)
{
  if (!image || !image->data || !image->capacity) return -EINVAL;
  image->size = image->width = image->height = 0;
#ifdef CAMERA_WARM_ENABLED
  nxmutex_lock(&g_camera_api_lock);
  nxmutex_lock(&g_camera.lock);
  int ret = !g_camera.paused || g_camera.reserved || g_camera.busy ? -EBUSY :
            g_camera.blocked ? g_camera.error : 0;
  nxmutex_unlock(&g_camera.lock);
  if (ret == 0)
    {
      ret = camera_capture(image, NULL);
      if (ret < 0)
        {
          /* Keep the first acquisition/cleanup error. A cold capture error
           * may include an unsuccessful final close: don't restart DMA. */
          nxmutex_lock(&g_camera.lock);
          g_camera.blocked = true; g_camera.error = ret;
          g_camera.resume_wanted = false;
          nxmutex_unlock(&g_camera.lock);
        }
    }
  nxmutex_unlock(&g_camera_api_lock);
  return ret;
#else
  return -ENOSYS;
#endif
}

int camera_service_capture_live(struct vision_badge_image_s *image,
                                unsigned int timeout_ms,
                                bool (*cancel)(void *), void *arg)
{
#ifdef CAMERA_WARM_ENABLED
  return camera_warm_snapshot(image, true, timeout_ms, cancel, arg);
#else
  (void)timeout_ms; (void)cancel; (void)arg;
  if (image) image->size = image->width = image->height = 0;
  return -ENOSYS;
#endif
}

int camera_service_control(const char *action, char *text, size_t capacity)
{
  if (!action || !text || !capacity) return -EINVAL;
#ifdef CAMERA_WARM_ENABLED
  nxmutex_lock(&g_camera_api_lock);
  int ret = 0;
  if (!strcmp(action, "start")) ret = camera_warm_start_locked();
  else if (!strcmp(action, "stop"))
    {
      nxmutex_lock(&g_camera.lock);
      if (g_camera.reserved) ret = -EBUSY;
      else g_camera.resume_wanted = false;
      nxmutex_unlock(&g_camera.lock);
      if (ret == 0) ret = camera_warm_stop_locked();
    }
  else if (!strcmp(action, "borrow"))
    {
      nxmutex_lock(&g_camera.lock);
      bool wanted = g_camera.wanted;
      nxmutex_unlock(&g_camera.lock);
      ret = camera_warm_start_locked();
      if (ret == 0)
        {
          nxmutex_lock(&g_camera.lock);
          g_camera.reserved = true; g_camera.resume_wanted = wanted;
          nxmutex_unlock(&g_camera.lock);
        }
    }
  else if (!strcmp(action, "pause"))
    {
      nxmutex_lock(&g_camera.lock);
      if (g_camera.paused || g_camera.reserved) ret = -EBUSY;
      else
        {
          g_camera.resume_wanted = g_camera.wanted;
          g_camera.paused = true;
        }
      nxmutex_unlock(&g_camera.lock);
      if (ret == 0)
        {
          ret = camera_warm_stop_locked();
          if (ret < 0)
            {
              /* A failed close cannot be followed by an audio takeover. */
              nxmutex_lock(&g_camera.lock);
              g_camera.blocked = true; g_camera.error = ret;
              g_camera.paused = g_camera.resume_wanted = false;
              nxmutex_unlock(&g_camera.lock);
            }
        }
    }
  else if (!strcmp(action, "resume"))
    {
      nxmutex_lock(&g_camera.lock);
      bool borrowed = g_camera.reserved;
      bool restart = g_camera.resume_wanted;
      if (!g_camera.paused && !borrowed) ret = -EINVAL;
      else g_camera.paused = g_camera.reserved = g_camera.resume_wanted = false;
      nxmutex_unlock(&g_camera.lock);
      if (ret == 0 && borrowed && !restart) ret = camera_warm_stop_locked();
      else if (ret == 0 && !borrowed && restart) ret = camera_warm_start_locked();
    }
  else if (strcmp(action, "status")) ret = -EINVAL;
  nxmutex_lock(&g_camera.lock);
  snprintf(text, capacity, "camera: enabled=%d active=%d ready=%d sequence=%lu age=%lu ms error=%d blocked=%d paused=%d reserved=%d",
           g_camera.wanted, g_camera.busy, g_camera.ready,
           (unsigned long)g_camera.sequence,
           (unsigned long)(g_camera.ready ? camera_now_ms() - g_camera.updated : 0),
           g_camera.error, g_camera.blocked, g_camera.paused, g_camera.reserved);
  nxmutex_unlock(&g_camera.lock);
  nxmutex_unlock(&g_camera_api_lock);
  return ret;
#else
  snprintf(text, capacity, "camera: warm mode not enabled");
  return -ENOSYS;
#endif
}

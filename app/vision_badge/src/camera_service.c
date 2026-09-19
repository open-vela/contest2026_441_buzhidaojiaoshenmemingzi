/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef __NuttX__
#  include <poll.h>
#  include <sys/ioctl.h>
#  include <sys/mman.h>
#  include <sys/videoio.h>
#endif

#include <vision_badge/config.h>
#include <vision_badge/services.h>

#ifndef O_NONBLOCK
#  define O_NONBLOCK 0
#endif

#ifdef __NuttX__
#  define VISION_BADGE_CAMERA_WIDTH       640
#  define VISION_BADGE_CAMERA_HEIGHT      480
#  define VISION_BADGE_CAMERA_BUFFERS     2
#  define VISION_BADGE_CAMERA_TIMEOUT_MS  5000

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
 *   5. 用 poll() 等待一帧就绪（最长等 5 秒）
 *   6. 取出帧缓冲（VIDIOC_DQBUF），验证 JPEG 头尾有效性
 *   7. 拷贝到用户提供的 image->data 缓冲区
 *
 * 清理（无论成功还是失败都执行）：
 *   - 停止视频流（VIDIOC_STREAMOFF）
 *   - munmap 所有缓冲区
 *   - 释放缓冲区申请（VIDIOC_REQBUFS count=0）
 *   - 关闭设备文件
 *
 * 硬件链路：GC2145 传感器 → DVP 并行接口 → YUV/DMA → PSRAM → JPEG 编码器
 *   实际由 BK7258 驱动层完成，这里只通过 V4L2 标准接口与驱动交互。
 * ======================================================================== */
int camera_service_capture(struct vision_badge_image_s *image)
{
  if (image == NULL || image->data == NULL || image->capacity == 0)
    {
      return -EINVAL;
    }

  image->size = 0;
  image->width = 0;
  image->height = 0;
  image->format = VISION_BADGE_IMAGE_JPEG;

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

  printf("vision_badge: opening %s\n",
         CONFIG_CONTEST2026_441_VISION_BADGE_CAMERA_DEVPATH);
  fflush(stdout);

  /* ---- 1. 打开摄像头设备 ---- */
  fd = open(CONFIG_CONTEST2026_441_VISION_BADGE_CAMERA_DEVPATH, O_RDWR);
  if (fd < 0)
    {
      return -errno;
    }

  printf("vision_badge: camera opened, configuring capture\n");
  fflush(stdout);

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

  /* ---- 4. 启动视频流 ---- */
  if (ioctl(fd, VIDIOC_STREAMON, (uintptr_t)&type) < 0)
    {
      ret = -errno;
      goto out;
    }

  printf("vision_badge: camera started, waiting up to %d ms for a frame\n",
         VISION_BADGE_CAMERA_TIMEOUT_MS);
  fflush(stdout);
  streaming = true;

  /* ---- 5. 用 poll() 等待一帧就绪（最长 5 秒） ---- */
  pollfd.fd = fd;
  pollfd.events = POLLIN;
  pollfd.revents = 0;

  do
    {
      poll_ret = poll(&pollfd, 1, VISION_BADGE_CAMERA_TIMEOUT_MS);
    }
  while (poll_ret < 0 && errno == EINTR);

  if (poll_ret == 0)
    {
      ret = -ETIMEDOUT;
      goto out;
    }

  if (poll_ret < 0)
    {
      ret = -errno;
      goto out;
    }

  /* ---- 6. 取出帧缓冲，验证 JPEG 有效性 ---- */
  memset(&buffer, 0, sizeof(buffer));
  buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_MMAP;

  if (ioctl(fd, VIDIOC_DQBUF, (uintptr_t)&buffer) < 0)
    {
      ret = -errno;
      goto out;
    }

  printf("vision_badge: camera frame received\n");
  fflush(stdout);

  /* 检查帧是否有效：索引范围、错误标志、数据长度 */
  if (buffer.index >= req.count ||
      (buffer.flags & V4L2_BUF_FLAG_ERROR) != 0 ||
      buffer.bytesused == 0 ||
      buffer.bytesused > mapped_length[buffer.index])
    {
      ret = -EIO;
      goto out;
    }

  if (buffer.bytesused > image->capacity)
    {
      ret = -EFBIG;
      goto out;
    }

  /* 验证 JPEG 头（FF D8）和尾（FF D9），以及 SOS 标记存在 */
  if (!camera_service_jpeg_valid(mapped[buffer.index], buffer.bytesused))
    {
      ret = -EBADMSG;
      goto out;
    }

  /* ---- 7. 拷贝 JPEG 数据到用户缓冲区 ---- */
  memcpy(image->data, mapped[buffer.index], buffer.bytesused);
  image->size = buffer.bytesused;
  image->width = fmt.fmt.pix.width;
  image->height = fmt.fmt.pix.height;

out:
  /* ---- 清理：停止流、unmap、释放缓冲、关闭设备 ---- */
  if (streaming && ioctl(fd, VIDIOC_STREAMOFF, (uintptr_t)&type) < 0 &&
      ret == 0)
    {
      ret = -errno;
    }

  for (i = 0; i < VISION_BADGE_CAMERA_BUFFERS; i++)
    {
      if (mapped[i] != NULL)
        {
          munmap(mapped[i], mapped_length[i]);
        }
    }

  if (buffers_requested)
    {
      req.count = 0;
      ioctl(fd, VIDIOC_REQBUFS, (uintptr_t)&req);
    }

  close(fd);
  if (ret < 0)
    {
      image->size = 0;
      image->width = 0;
      image->height = 0;
    }

  return ret;
#else
  return -ENOSYS;
#endif
}

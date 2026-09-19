/****************************************************************************
 * boards/bk7258/aidk_ai_toy/src/bk7258_aidk_bsp_selftest.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bounded, non-product AP-side validation for the R1 board data paths.
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_BK7258_AIDK_BSP_SELFTEST

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/videoio.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/kmalloc.h>

#include <arch/board/board.h>

#define AIDK_BSP_CAMERA_TIMEOUT_MS       3000
#define AIDK_BSP_CAMERA_MAX_SIZE         (1024u * 1024u)
#define AIDK_BSP_SD_MOUNTPOINT           "/mnt/sdnand"
#define AIDK_BSP_SD_TESTFILE              "/mnt/sdnand/.bsp-r1-test"
#define AIDK_BSP_SD_TEST_TEXT             "bk7258-r1-bsp\n"

extern int bk7258_aidk_dual_lcd_data_selftest(void);

static int aidk_bsp_errno(void)
{
  return errno > 0 ? -errno : -EIO;
}

static int aidk_bsp_lcd_test(void)
{
#ifdef CONFIG_BK7258_AIDK_DUAL_LCD
  int ret = bk7258_aidk_dual_lcd_data_selftest();

  if (ret < 0)
    {
      syslog(LOG_ERR, "AIDK BSP LCD DATA FAIL ret=%d\n", ret);
      return ret;
    }

  return OK;
#else
  syslog(LOG_INFO, "AIDK BSP LCD DATA SKIP disabled\n");
  return OK;
#endif
}

static int aidk_bsp_camera_test(void)
{
#ifdef CONFIG_BK7258_AIDK_CAMERA
  struct v4l2_capability capability;
  struct v4l2_format format;
  struct v4l2_requestbuffers request;
  struct v4l2_buffer buffer;
  enum v4l2_buf_type type;
  struct pollfd pollfd;
  FAR uint8_t *capture = NULL;
  uint32_t capture_size;
  int fd = -1;
  int ret;
  bool stream_on = false;

  fd = open(BK7258_BOARD_CAMERA_DEVPATH, O_RDWR | O_NONBLOCK);
  if (fd < 0)
    {
      ret = aidk_bsp_errno();
      goto errout;
    }

  memset(&capability, 0, sizeof(capability));
  if (ioctl(fd, VIDIOC_QUERYCAP,
            (unsigned long)(uintptr_t)&capability) < 0)
    {
      ret = aidk_bsp_errno();
      goto errout;
    }

  if ((capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0)
    {
      ret = -ENOTSUP;
      goto errout;
    }

  memset(&format, 0, sizeof(format));
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  format.fmt.pix.width = BK7258_BOARD_CAMERA_WIDTH;
  format.fmt.pix.height = BK7258_BOARD_CAMERA_HEIGHT;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  if (ioctl(fd, VIDIOC_S_FMT,
            (unsigned long)(uintptr_t)&format) < 0)
    {
      ret = aidk_bsp_errno();
      goto errout;
    }

  capture_size = format.fmt.pix.sizeimage;
  syslog(LOG_INFO, "AIDK BSP CAMERA FORMAT %lux%lu fourcc=0x%08lx sizeimage=%lu\n",
         (unsigned long)format.fmt.pix.width,
         (unsigned long)format.fmt.pix.height,
         (unsigned long)format.fmt.pix.pixelformat,
         (unsigned long)capture_size);

  /* The pinned v4l2_cap.c capture_s_fmt() preserves sizeimage=0.
   * Its get_bufsize() uses width * height for JPEG in that case.
   * Match that upper-half contract for our USERPTR buffer, rather than
   * treating an unspecified sizeimage as a failed camera capture.
   */

  if (capture_size == 0 &&
      format.fmt.pix.pixelformat == V4L2_PIX_FMT_JPEG)
    {
      if (format.fmt.pix.width == 0 || format.fmt.pix.height == 0 ||
          format.fmt.pix.width >
          AIDK_BSP_CAMERA_MAX_SIZE / format.fmt.pix.height)
        {
          ret = -EOVERFLOW;
          goto errout;
        }

      capture_size = format.fmt.pix.width * format.fmt.pix.height;
      syslog(LOG_INFO,
             "AIDK BSP CAMERA BUFFER source=nuttx-jpeg-default bytes=%lu\n",
             (unsigned long)capture_size);
    }

  if (capture_size == 0 || capture_size > AIDK_BSP_CAMERA_MAX_SIZE)
    {
      ret = -EOVERFLOW;
      goto errout;
    }

  memset(&request, 0, sizeof(request));
  request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  request.memory = V4L2_MEMORY_USERPTR;
  /* A single capture buffer must use FIFO mode.  The pinned NuttX
   * video_framebuff implementation advances the producer/consumer pointer
   * only after a FIFO buffer is completed; with the default RING mode and
   * one buffer, the ring points back to itself and DQBUF never becomes
   * ready (the driver reports EAGAIN). */
  request.mode = V4L2_BUF_MODE_FIFO;
  request.count = 1;
  if (ioctl(fd, VIDIOC_REQBUFS,
            (unsigned long)(uintptr_t)&request) < 0 ||
      request.count != 1)
    {
      ret = aidk_bsp_errno();
      goto errout;
    }

  if (request.mode != V4L2_BUF_MODE_FIFO)
    {
      ret = -EPROTO;
      goto errout;
    }

  capture = kmm_memalign(32, capture_size);
  if (capture == NULL)
    {
      ret = -ENOMEM;
      goto errout;
    }

  memset(&buffer, 0, sizeof(buffer));
  buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_USERPTR;
  buffer.index = 0;
  buffer.m.userptr = (unsigned long)(uintptr_t)capture;
  buffer.length = capture_size;
  if (ioctl(fd, VIDIOC_QBUF,
            (unsigned long)(uintptr_t)&buffer) < 0)
    {
      ret = aidk_bsp_errno();
      goto errout;
    }

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(fd, VIDIOC_STREAMON, (unsigned long)(uintptr_t)&type) < 0)
    {
      ret = aidk_bsp_errno();
      goto errout;
    }

  stream_on = true;
  pollfd.fd = fd;
  pollfd.events = POLLIN;
  pollfd.revents = 0;
  ret = poll(&pollfd, 1, AIDK_BSP_CAMERA_TIMEOUT_MS);
  if (ret <= 0)
    {
      ret = ret == 0 ? -ETIMEDOUT : aidk_bsp_errno();
      goto errout;
    }

  memset(&buffer, 0, sizeof(buffer));
  buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_USERPTR;
  if (ioctl(fd, VIDIOC_DQBUF,
            (unsigned long)(uintptr_t)&buffer) < 0)
    {
      ret = aidk_bsp_errno();
      goto errout;
    }

  if (buffer.bytesused < 4 || buffer.bytesused > capture_size ||
      capture[0] != 0xff || capture[1] != 0xd8 ||
      capture[buffer.bytesused - 2] != 0xff ||
      capture[buffer.bytesused - 1] != 0xd9)
    {
      ret = -EBADMSG;
      goto errout;
    }

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  (void)ioctl(fd, VIDIOC_STREAMOFF, (unsigned long)(uintptr_t)&type);
  stream_on = false;
  syslog(LOG_INFO, "AIDK BSP CAMERA DATA PASS bytes=%lu\n",
         (unsigned long)buffer.bytesused);
  kmm_free(capture);
  close(fd);
  return OK;

errout:
  if (stream_on)
    {
      type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      (void)ioctl(fd, VIDIOC_STREAMOFF, (unsigned long)(uintptr_t)&type);
    }

  if (capture != NULL)
    {
      kmm_free(capture);
    }

  if (fd >= 0)
    {
      close(fd);
    }

  syslog(LOG_ERR, "AIDK BSP CAMERA DATA FAIL ret=%d\n", ret);
  return ret;
#else
  syslog(LOG_INFO, "AIDK BSP CAMERA DATA SKIP disabled\n");
  return OK;
#endif
}

static int aidk_bsp_storage_test(void)
{
#ifdef CONFIG_BK7258_AIDK_SD_NAND
  struct stat status;
  char readback[sizeof(AIDK_BSP_SD_TEST_TEXT)];
  FAR const char *device = NULL;
  int fd = -1;
  int ret;
  bool mounted = false;

  if (stat("/dev/mmcsd0p0", &status) == 0)
    {
      device = "/dev/mmcsd0p0";
    }
  else if (stat("/dev/mmcsd0", &status) == 0)
    {
      device = "/dev/mmcsd0";
      syslog(LOG_INFO,
             "AIDK BSP SD DATA using raw block node; partition node absent\n");
    }
  else
    {
      if (errno == ENOENT)
        {
          syslog(LOG_INFO, "AIDK BSP SD DATA SKIP device-missing\n");
          return OK;
        }

      ret = aidk_bsp_errno();
      goto errout;
    }

  (void)mkdir(AIDK_BSP_SD_MOUNTPOINT, 0777);
  if (mount(device, AIDK_BSP_SD_MOUNTPOINT, "vfat", 0,
            NULL) == 0)
    {
      mounted = true;
    }
  else if (errno != EBUSY)
    {
      ret = aidk_bsp_errno();
      goto errout;
    }

  fd = open(AIDK_BSP_SD_TESTFILE, O_CREAT | O_RDWR | O_TRUNC, 0666);
  if (fd < 0)
    {
      ret = aidk_bsp_errno();
      goto errout;
    }

  if (write(fd, AIDK_BSP_SD_TEST_TEXT,
            sizeof(AIDK_BSP_SD_TEST_TEXT) - 1) !=
      (ssize_t)(sizeof(AIDK_BSP_SD_TEST_TEXT) - 1) ||
      lseek(fd, 0, SEEK_SET) < 0 ||
      read(fd, readback, sizeof(AIDK_BSP_SD_TEST_TEXT) - 1) !=
      (ssize_t)(sizeof(AIDK_BSP_SD_TEST_TEXT) - 1) ||
      memcmp(readback, AIDK_BSP_SD_TEST_TEXT,
             sizeof(AIDK_BSP_SD_TEST_TEXT) - 1) != 0)
    {
      ret = aidk_bsp_errno();
      goto errout;
    }

  close(fd);
  fd = -1;
  (void)unlink(AIDK_BSP_SD_TESTFILE);
  if (mounted)
    {
      (void)umount(AIDK_BSP_SD_MOUNTPOINT);
    }

  syslog(LOG_INFO, "AIDK BSP SD DATA PASS\n");
  return OK;

errout:
  if (fd >= 0)
    {
      close(fd);
    }

  if (mounted)
    {
      (void)umount(AIDK_BSP_SD_MOUNTPOINT);
    }

  syslog(LOG_ERR, "AIDK BSP SD DATA FAIL ret=%d\n", ret);
  return ret;
#else
  syslog(LOG_INFO, "AIDK BSP SD DATA SKIP disabled\n");
  return OK;
#endif
}

int bk7258_aidk_bsp_selftest(void)
{
  unsigned int failures = 0;

  if (aidk_bsp_lcd_test() < 0)
    {
      failures++;
    }

  if (aidk_bsp_camera_test() < 0)
    {
      failures++;
    }

  if (aidk_bsp_storage_test() < 0)
    {
      failures++;
    }

  syslog(failures == 0 ? LOG_INFO : LOG_WARNING,
         "AIDK BSP SELFTEST %s failures=%u\n",
         failures == 0 ? "PASS" : "FAIL", failures);
  return failures == 0 ? OK : -EIO;
}

#endif /* CONFIG_BK7258_AIDK_BSP_SELFTEST */

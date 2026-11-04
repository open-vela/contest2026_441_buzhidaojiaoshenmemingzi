# SPDX-License-Identifier: Apache-2.0
"""Configure only the authenticated LAN wake connection, never OS settings.

Windows delayed ACKs can exhaust the embedded sender's small write pool.
SIO_TCP_SET_ACK_FREQUENCY is the native, per-socket control; value 1 asks
for an ACK per packet. Closing this socket also removes the setting.
"""
import ctypes
from functools import lru_cache
import sys

SIO_TCP_SET_ACK_FREQUENCY = 2550136855


@lru_cache(maxsize=1)
def _windows_functions():
    winsock = ctypes.WinDLL("Ws2_32.dll")
    ioctl = winsock.WSAIoctl
    ioctl.argtypes = [ctypes.c_size_t, ctypes.c_uint32, ctypes.c_void_p,
                      ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32,
                      ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p, ctypes.c_void_p]
    ioctl.restype = ctypes.c_int
    last_error = winsock.WSAGetLastError
    last_error.argtypes = []
    last_error.restype = ctypes.c_int
    return ioctl, last_error


def configure_wake_stream(connection):
    if sys.platform != "win32":
        return False
    ioctl, last_error = _windows_functions()
    frequency, returned = ctypes.c_uint32(1), ctypes.c_uint32()
    result = ioctl(connection.fileno(), SIO_TCP_SET_ACK_FREQUENCY,
                   ctypes.byref(frequency), ctypes.sizeof(frequency),
                   None, 0, ctypes.byref(returned), None, None)
    if result:
        raise OSError(last_error(), "wake TCP ACK configuration failed")
    return True

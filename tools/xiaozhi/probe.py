#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Silent Xiaozhi v1 hello probe: no microphone, speaker or model request.

This is a host-side network diagnostic, not the R1 firmware client. Use the
same protocol contract when integrating the NuttX client. The example device
ID is a host probe ID; it must not be confused with the board's hardware ID.
"""

import argparse
import asyncio
import json
from pathlib import Path
import re
from urllib.parse import urlsplit

from websockets.asyncio.client import connect


def hello():
    # Do not advertise MCP or AEC until the R1 implementation actually exists.
    return {"type": "hello", "version": 1, "transport": "websocket",
            "audio_params": {"format": "opus", "sample_rate": 16000,
                             "channels": 1, "frame_duration": 60}}


def validate_config(config):
    url = urlsplit(config.get("url", ""))
    if url.scheme not in {"ws", "wss"} or not url.hostname or url.username or url.password:
        raise ValueError("url must be ws:// or wss:// without embedded credentials")
    if url.fragment:
        raise ValueError("WebSocket URL must not contain a fragment")
    if not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", config.get("device_id", "")):
        raise ValueError("device_id must be a colon-separated MAC-format ID")
    for field in ("client_id", "token"):
        value = config.get(field, "")
        if not isinstance(value, str) or "\r" in value or "\n" in value:
            raise ValueError(f"invalid {field}")
    if not config.get("client_id"):
        raise ValueError("client_id is required")
    timeout = config.get("timeout_seconds", 10)
    if type(timeout) not in (int, float) or not 0 < timeout <= 60:
        raise ValueError("timeout_seconds must be between 0 and 60")
    return timeout


def validate_hello(payload):
    if not isinstance(payload, str):
        raise ValueError("expected text hello, received binary before handshake")
    reply = json.loads(payload)
    if not isinstance(reply, dict) or reply.get("type") != "hello" or reply.get("transport") != "websocket":
        raise ValueError("server did not acknowledge WebSocket hello")
    session = reply.get("session_id", "")
    if not isinstance(session, str) or not session or len(session) > 128:
        raise ValueError("session_id is missing or too long for the R1 session contract")
    params = reply.get("audio_params", hello()["audio_params"])
    if not isinstance(params, dict):
        raise ValueError("invalid audio_params")
    params = {**hello()["audio_params"], **params}
    if params["format"] != "opus" or type(params["channels"]) is not int or params["channels"] != 1:
        raise ValueError("first R1 client supports mono Opus only")
    if type(params["sample_rate"]) is not int or params["sample_rate"] not in (16000, 24000):
        raise ValueError("unsupported server sample rate")
    if type(params["frame_duration"]) is not int or params["frame_duration"] != 60:
        raise ValueError("initial protocol contract requires 60 ms frames")
    return {"session_id": session, "audio_params": params}


async def probe(config):
    timeout = validate_config(config)
    headers = {"Protocol-Version": "1", "Device-Id": config["device_id"],
               "Client-Id": config["client_id"]}
    if config.get("token"):
        headers["Authorization"] = "Bearer " + config["token"]
    async with connect(config["url"], additional_headers=headers,
                       open_timeout=timeout, close_timeout=2,
                       max_size=8192, ping_interval=20, proxy=None) as websocket:
        await asyncio.wait_for(websocket.send(json.dumps(hello())), timeout)
        payload = await asyncio.wait_for(websocket.recv(), timeout)
        return validate_hello(payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    args = parser.parse_args()
    try:
        config = json.loads(args.config.read_text(encoding="utf-8"))
        if not isinstance(config, dict):
            raise ValueError("config must be an object")
        result = asyncio.run(probe(config))
    except Exception as error:
        # Provider errors/URLs can contain secrets; don't print raw exception text.
        print(f"小智静默握手失败：{type(error).__name__}；检查服务、配置和协商格式。")
        return 1
    params = result["audio_params"]
    print(f"小智静默握手通过：Opus {params['sample_rate']}Hz / 单声道 / 60ms")
    print("仅证明主机与服务端握手；没有采音、播放、模型调用或 R1 实机验证。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

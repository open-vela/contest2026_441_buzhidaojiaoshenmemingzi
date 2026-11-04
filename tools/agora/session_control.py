"""R1 local RTC token preparation and bounded, silent cloud-agent probe.

Token reference: AgoraIO-Community/Conversational-AI-IOT-Sample
bk7258/v2.0.1, commit 2e736ebefdb7ffb2832ae1a6f95039d8589e1f85 (MIT).
Account credentials stay in R1/config; never put certificates on the device.
"""
import argparse
import base64
import json
import os
import re
from pathlib import Path
import sys
import time
import urllib.error
import urllib.request
from urllib.parse import urlparse
import uuid

ROOT = Path(os.environ.get("R1_LOCAL_ROOT", Path(__file__).resolve().parents[4]))
sys.path.insert(0, os.environ.get("AGORA_TOKEN_REFERENCE", str(ROOT / "tools" / "agora-token-reference")))
from RtcTokenBuilder2 import RtcTokenBuilder, Role_Publisher


def account():
    result = {}
    for line in (ROOT / "config" / "agora.env").read_text(encoding="utf-8-sig").splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            key, value = line.split("=", 1)
            result[key.strip()] = value.strip().strip("\"'")
    for key in ("AGORA_APP_ID", "AGORA_APP_CERTIFICATE", "AGORA_CUSTOMER_ID", "AGORA_CUSTOMER_SECRET"):
        if not result.get(key):
            raise ValueError("Missing configuration field: " + key)
    return result


def token(cfg, channel, uid, ttl):
    return RtcTokenBuilder.build_token_with_uid(
        cfg["AGORA_APP_ID"], cfg["AGORA_APP_CERTIFICATE"], channel,
        uid, Role_Publisher, ttl, ttl)


def llm_config():
    provider = json.loads((ROOT / "config" / "voice-provider.local.json").read_text(encoding="utf-8-sig"))
    url = provider["base_url"].rstrip("/")
    if not url.endswith("/chat/completions"):
        url += "/chat/completions"
    if urlparse(url).scheme != "https":
        raise ValueError("LLM requires HTTPS")
    return {"url": url, "api_key": provider["api_key"], "vendor": "openai",
            "params": {"model": provider["model"], "stream": True, "max_tokens": 120},
            "max_history": 12,
            "system_messages": [{"role": "system", "content": "你是R1语音伙伴。用自然、简短的中文回答，每次优先一两句话。记住本次会话的信息。不知道就说明不知道，不编造天气或其他实时事实。"}],
            "greeting_message": "你好，我在。"}


def free_llm_config():
    llm = llm_config()
    if (llm["params"]["model"] != "glm-4-flash-250414" or
            llm["url"] != "https://open.bigmodel.cn/api/paas/v4/chat/completions"):
        raise ValueError("free-only conversation requires the checked free GLM model")
    return llm


def free_voice_config():
    """Explicit providers; no inherited paid pipeline or automatic fallback."""
    from mimo_tts_bridge import agora_voice_config
    llm = free_llm_config()
    properties = agora_voice_config()
    properties["llm"] = llm
    return properties


def request(cfg, suffix, method="GET", body=None):
    auth = (cfg["AGORA_CUSTOMER_ID"] + ":" + cfg["AGORA_CUSTOMER_SECRET"]).encode()
    prefix = "/cn/api" if cfg.get("R1_REGION", "cn") == "cn" else "/api"
    url = "https://api.agora.io" + prefix + "/conversational-ai-agent/v2/projects/" + cfg["AGORA_APP_ID"] + suffix
    req = urllib.request.Request(url, method=method,
          data=None if body is None else json.dumps(body).encode(),
          headers={"Authorization": "Basic " + base64.b64encode(auth).decode(),
                   "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=20) as response:
            data = response.read(65537)
            if len(data) > 65536:
                raise ValueError("Oversized API response")
            print("Agora", method, suffix.split("/")[1], "HTTP", response.status)
            return json.loads(data) if data else {}
    except urllib.error.HTTPError as error:
        # No raw server body: it can echo tokens, resource IDs or provider keys.
        raw = error.read(65536)
        try:
            result = json.loads(raw)
            code = result.get("code", result.get("error_code", "unspecified"))
            if not isinstance(code, (str, int)) or len(str(code)) > 80:
                code = "unspecified"
            message = result.get("message", result.get("detail", result.get("error", "")))
            if isinstance(message, dict):
                message = message.get("message", message.get("reason", ""))
            if not isinstance(message, str):
                message = ""
            for secret in cfg.values():
                if secret:
                    message = message.replace(secret, "[redacted]")
            message = re.sub(r"[A-Za-z0-9_./+=-]{24,}", "[redacted]", message)
            message = message[:400]
        except (ValueError, TypeError):
            code = "non-json"
            message = ""
        raise RuntimeError(f"Agora HTTP {error.code}, error code={code}, reason={message}") from None


def read_text_stream(response, started):
    """Validate bounded OpenAI-compatible SSE without microphone or playback."""
    fragments, event = [], []
    total, first = 0, None
    while True:
        if time.monotonic() - started > 30:
            raise TimeoutError("text stream exceeded 30 seconds")
        line = response.readline(65537)
        if len(line) > 65536:
            raise ValueError("oversized stream line")
        total += len(line)
        if total > 1048576:
            raise ValueError("oversized text stream")
        if not line:
            raise ValueError("text stream ended without DONE")
        line = line.decode("utf-8").rstrip("\r\n")
        if line.startswith("data:"):
            event.append(line[5:].lstrip(" "))
        elif not line and event:
            data = "\n".join(event)
            event = []
            if data == "[DONE]":
                if not fragments:
                    raise ValueError("text stream returned no content")
                return "".join(fragments), first, len(fragments)
            message = json.loads(data)
            if not isinstance(message, dict) or "error" in message:
                raise ValueError("text stream returned an error event")
            choices = message.get("choices", [])
            if not isinstance(choices, list):
                raise ValueError("invalid stream choices")
            if choices:
                if not isinstance(choices[0], dict) or not isinstance(choices[0].get("delta", {}), dict):
                    raise ValueError("invalid stream delta")
                content = choices[0].get("delta", {}).get("content")
                if content is not None and not isinstance(content, str):
                    raise ValueError("invalid stream content")
                if content:
                    if first is None: first = round((time.monotonic() - started) * 1000)
                    fragments.append(content)


def provider_stream_test():
    llm = free_llm_config()
    messages = [{"role": "system", "content": "你是一个中文语音助手，回答简短。"},
                {"role": "user", "content": "接下来请叫我青松，记住这个称呼。只回答好的。"}]
    for turn in (1, 2):
        payload = {"model": llm["params"]["model"], "stream": True,
                   "max_tokens": 32, "messages": messages}
        req = urllib.request.Request(llm["url"], data=json.dumps(payload).encode(),
              headers={"Authorization": "Bearer " + llm["api_key"], "Content-Type": "application/json"})
        started = time.monotonic()
        try:
            with urllib.request.urlopen(req, timeout=20) as response:
                if "text/event-stream" not in response.headers.get("Content-Type", "").lower():
                    raise ValueError("provider did not return a text event stream")
                text, first, chunks = read_text_stream(response, started)
                if turn == 2 and "青松" not in text:
                    raise ValueError("provider did not recall supplied test context")
                print("Provider stream", "turn=" + str(turn), "HTTP=" + str(response.status),
                      "first_text_ms=" + str(first), "total_ms=" + str(round((time.monotonic() - started) * 1000)),
                      "chunks=" + str(chunks), "chars=" + str(len(text)),
                      "context_recall=" + ("PASS" if turn == 2 else "not_checked"))
                messages += [{"role": "assistant", "content": text},
                             {"role": "user", "content": "我刚刚让你用什么称呼？只说称呼。"}]
        except urllib.error.HTTPError as error:
            raise RuntimeError("Language model HTTP " + str(error.code)) from None


def summarize_session(info, page):
    """Only diagnostic counts/codes; never return transcript, channel or keys."""
    if not isinstance(info, dict) or not isinstance(page, dict):
        raise ValueError("invalid diagnostic response")
    info, page = info.get("data", info), page.get("data", page)
    if not isinstance(info, dict) or not isinstance(page, dict):
        raise ValueError("invalid diagnostic response data")
    status = info.get("status")
    if status not in ("IDLE", "STARTING", "RUNNING", "STOPPING", "STOPPED", "RECOVERING", "FAILED"):
        raise ValueError("unknown session status; no cleanup success assumed")
    total, turns = page.get("total_turn_count"), page.get("turns")
    if type(total) is not int or total < 0 or not isinstance(turns, list) or len(turns) > 200:
        raise ValueError("invalid turn count/page")
    counts = {k: 0 for k in ("voice_input", "greeting", "silence_timeout", "api_speak", "api_think", "other")}
    reasons, codes = {}, {}
    missing_prompt = 0
    for turn in turns:
        if not isinstance(turn, dict) or not isinstance(turn.get("start"), dict) or not isinstance(turn.get("end"), dict):
            raise ValueError("invalid turn metadata")
        kind = turn["start"].get("type")
        counts[kind if isinstance(kind, str) and kind in counts else "other"] += 1
        metadata = turn["end"].get("metadata", {})
        if not isinstance(metadata, dict): raise ValueError("invalid end metadata")
        reason = metadata.get("reason")
        if isinstance(reason, str):
            safe_reason = reason if reason in ("LLM_REQUEST_ERR", "TTS_REQUEST_ERR", "ASR_REQUEST_ERR") else "OTHER"
            reasons[safe_reason] = reasons.get(safe_reason, 0) + 1
        details = metadata.get("details")
        if isinstance(details, str):
            try: details = json.loads(details)
            except (ValueError, TypeError): details = None
        if isinstance(details, dict) and isinstance(details.get("error"), dict):
            code = details["error"].get("code")
            if isinstance(code, (str, int)) and re.fullmatch(r"[0-9]{1,8}", str(code)):
                codes[str(code)] = codes.get(str(code), 0) + 1
            message = details["error"].get("message")
            if str(code) == "1213" and isinstance(message, str) and "prompt" in message.lower():
                missing_prompt += 1
    return {"status": status, "total_turn_count": total, "page_turns": len(turns),
            "turn_types": counts, "error_reasons": reasons, "provider_codes": codes,
            "missing_prompt_errors": missing_prompt,
            "limits": "page counts only; API acceptance/turn metadata do not prove board audio or zero billing"}


def inspect_session(cfg, agent_id):
    if not isinstance(agent_id, str) or not re.fullmatch(r"[A-Za-z0-9_-]{16,128}", agent_id):
        raise ValueError("invalid agent identifier")
    path = "/agents/" + agent_id
    return summarize_session(request(cfg, path), request(cfg, path + "/turns?page_index=1&page_size=50"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("prepare", "probe", "provider-test", "provider-stream-test", "inspect"))
    parser.add_argument("--agent-id", help="existing instance for read-only status/turn inspection")
    parser.add_argument("--ttl", type=int, default=3600)
    parser.add_argument("--region", choices=("cn", "global"), default="cn")
    args = parser.parse_args()
    if args.action == "inspect":
        cfg = account(); cfg["R1_REGION"] = args.region
        print(json.dumps(inspect_session(cfg, args.agent_id), ensure_ascii=False))
        return  # No token minting, device-config overwrite, join or leave.
    if not 60 <= args.ttl <= 86400:
        parser.error("ttl must be 60..86400 seconds")
    if args.action == "provider-stream-test":
        provider_stream_test()
        return
    if args.action == "provider-test":
        llm = free_llm_config()
        payload = {"model": llm["params"]["model"], "max_tokens": 16,
                   "messages": [{"role": "user", "content": "请回复一个你好。"}]}
        req = urllib.request.Request(llm["url"], data=json.dumps(payload).encode(),
              headers={"Authorization": "Bearer " + llm["api_key"], "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=20) as response:
                answer = json.load(response)
                if not answer.get("choices"): raise ValueError("LLM returned no choices")
                print("Current language model HTTP", response.status, "model=" + payload["model"])
        except urllib.error.HTTPError as error:
            raise RuntimeError("Language model HTTP " + str(error.code)) from None
        return
    voice = free_voice_config() if args.action == "probe" else None
    cfg = account()
    cfg["R1_REGION"] = args.region
    channel = "r1_" + uuid.uuid4().hex[:16]
    device_uid, agent_uid = 1, 2
    device = {"app_id": cfg["AGORA_APP_ID"], "channel": channel,
              "uid": device_uid, "remote_uid": agent_uid,
              "token": token(cfg, channel, device_uid, args.ttl),
              "expires_at": int(time.time()) + args.ttl, "volume": 55}
    output = ROOT / "config" / "agora.cfg"
    output.write_text(json.dumps(device), encoding="utf-8")
    print("Device config prepared:", output, "TTL", args.ttl, "seconds; no certificate on device")
    if args.action == "prepare":
        return
    body = {"name": "r1_silent_probe_" + uuid.uuid4().hex,
            "properties": {"channel": channel, "agent_rtc_uid": str(agent_uid),
                "remote_rtc_uids": [str(device_uid)], "enable_string_uid": False,
                "token": token(cfg, channel, agent_uid, args.ttl), "idle_timeout": 10,
                **voice}}
    agent_id = None
    try:
        result = request(cfg, "/join", "POST", body)
        agent_id = result.get("agent_id")
        if not isinstance(agent_id, str) or not agent_id:
            raise ValueError("Join returned no agent_id; inspect account dashboard for unexpected agent")
        print("Cloud agent created; no R1 audio device started")
        time.sleep(2)
        result = request(cfg, "/agents/" + agent_id)
        status = result.get("status", result.get("state", "not exposed"))
        print("Cloud agent status:", status)
    finally:
        if agent_id:
            # Always close this probe. Idle timeout also bounds failed cleanup.
            request(cfg, "/agents/" + agent_id + "/leave", "POST", {})
            print("Cloud probe stopped")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        # Error text above contains no secrets; OS/network errors are generic.
        print(type(error).__name__ + ": " + str(error), file=sys.stderr)
        sys.exit(1)

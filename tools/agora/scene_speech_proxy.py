"""Opt-in OpenAI-compatible callback for trusted ordinary scene results.

Reuse Agora's LLM callback and existing TTS/history. Ordinary chat/tool
selection still streams from the reviewed GLM provider. Only an exact result
of this lease's latest successful camera tool can skip another paraphrase.
No model implementation, recorder, new public path or voice-provider switch.
"""
import copy
import hmac
import http.client
import json
import secrets
import threading
import time
from urllib.parse import urlsplit

PREFIX = 'r1scene-'


class SceneSpeechProxy:
    def __init__(self, broker, connection_factory=http.client.HTTPSConnection):
        self.broker, self.connection_factory = broker, connection_factory
        self.lock = threading.Lock()
        self.active = None

    def activate(self, lease, provider, end):
        url = urlsplit(provider.get('url', ''))
        if (url.scheme != 'https' or url.hostname != 'open.bigmodel.cn' or
                url.port not in (None, 443) or url.path != '/api/paas/v4/chat/completions' or
                url.query or url.fragment or url.username or url.password or
                provider.get('params', {}).get('model') != 'glm-4-flash-250414' or
                not isinstance(provider.get('api_key'), str) or not provider['api_key']):
            raise ValueError('reviewed GLM callback provider required')
        with self.lock:
            if self.active is not None:
                raise RuntimeError('scene speech callback already active')
            token = PREFIX + secrets.token_hex(32)
            self.active = dict(lease=lease, token=token, provider=copy.deepcopy(provider), end=end)
            return token

    def close(self, lease):
        with self.lock:
            if self.active and self.active['lease'] == lease:
                self.active = None

    def authorize(self, authorization):
        with self.lock:
            current = self.active
            if (not current or time.monotonic() >= current['end'] or
                    not isinstance(authorization, str) or
                    not hmac.compare_digest(authorization.encode(), ('Bearer '+current['token']).encode())):
                raise PermissionError('inactive scene speech credential')
            return copy.deepcopy(current)

    def direct_text(self, current, messages):
        # Any safety/route/distance/reading question keeps the original model
        # path. Do not replace a nuanced answer or truncate visible text.
        users = [m.get('content') for m in messages if m.get('role') == 'user']
        if not users or not isinstance(users[-1], str):
            return None
        if any(word in users[-1] for word in ('安全','距离','几米','多远','怎么走','能走','走过去','往哪','路线','导航','方向','左转','右转','可以','能不能','读','文字','牌','上写')):
            return None
        if messages[-1].get('role') != 'tool':
            return None
        last = messages[-1]
        if not isinstance(last.get('content'), str):
            return None
        try:
            result = json.loads(last['content'])
        except (ValueError, TypeError):
            return None
        with self.broker.condition:
            job = self.broker.job
            if (not self.broker._live() or self.broker.active[0] != current['lease'] or
                    not job or time.monotonic() >= job['deadline'] or last.get('tool_call_id') != job['call_id'] or
                    result != job.get('result') or not isinstance(result, dict) or
                    result.get('ok') is not True or result.get('navigation') is not False):
                return None
            # The preceding assistant must really have called this function;
            # user text containing a JSON-shaped result cannot select it.
            if len(messages) < 2 or messages[-2].get('role') != 'assistant':
                return None
            calls = messages[-2].get('tool_calls', [])
            if (not isinstance(calls, list) or not any(isinstance(call, dict) and
                    call.get('id') == job['call_id'] and
                    isinstance(call.get('function'), dict) and
                    call['function'].get('name') == 'observeScene' for call in calls)):
                return None
            text = result.get('text')
            if not isinstance(text, str) or not text.strip() or len(text.encode()) > 240:
                return None
            if any(word in text for word in ('安全','放心走','行走','通行','向前走','往左走','往右走','转弯','前进','后退','米')):
                return None
            return text

    @staticmethod
    def headers(handler):
        handler.close_connection = True
        handler.send_response(200)
        handler.send_header('Content-Type', 'text/event-stream; charset=utf-8')
        handler.send_header('Cache-Control', 'no-cache')
        handler.send_header('Connection', 'close')
        handler.end_headers()

    def handle(self, handler):
        sent = False
        started = time.monotonic()
        try:
            auth = handler.headers.get('Authorization', '')
            current = self.authorize(auth)  # Before reading any public body.
            size = int(handler.headers.get('Content-Length', '0'))
            if not 1 <= size <= 65536 or handler.headers.get('Transfer-Encoding'):
                raise ValueError('invalid callback body size')
            raw = handler.rfile.read(size)
            if len(raw) != size:
                raise ValueError('incomplete callback body')
            body = json.loads(raw)
            messages = body.get('messages') if isinstance(body, dict) else None
            if (not isinstance(messages, list) or not 1 <= len(messages) <= 128 or
                    any(not isinstance(m, dict) or m.get('role') not in ('system','user','assistant','tool') for m in messages) or
                    body.get('stream') is not True or
                    body.get('model') != current['provider']['params']['model']):
                raise ValueError('invalid callback conversation')
            self.authorize(auth)
            direct = self.direct_text(current, messages)
            # Schema only: diagnose the deployed callback without logging
            # conversation text, tool bodies, identifiers or credentials.
            users = [m for m in messages if m.get('role') == 'user']
            print('R1_SCENE_CALLBACK '+json.dumps(dict(
                unix_ms=int(time.time()*1000), roles=[m['role'] for m in messages[-3:]],
                last_content_type=type(messages[-1].get('content')).__name__,
                user_content_type=type(users[-1].get('content')).__name__ if users else 'absent',
                direct=direct is not None)),flush=True)
            if direct is not None:
                self.authorize(auth)
                self.headers(handler);sent = True
                chunk = dict(id='scene-'+secrets.token_hex(8), object='chat.completion.chunk',
                             created=int(time.time()), model=body['model'],
                             choices=[dict(index=0, delta=dict(role='assistant',content=direct), finish_reason=None)])
                handler.wfile.write(b'data: '+json.dumps(chunk,ensure_ascii=False,separators=(',',':')).encode()+b'\n\n')
                finish = {**chunk,'choices':[dict(index=0,delta={},finish_reason='stop')]}
                handler.wfile.write(b'data: '+json.dumps(finish,separators=(',',':')).encode()+b'\n\ndata: [DONE]\n\n')
                handler.wfile.flush()
                print('R1_SCENE_SPEECH '+json.dumps(dict(mode='direct',characters=len(direct),
                      elapsed_ms=round((time.monotonic()-started)*1000))),flush=True)
                return
            # Relay the provider's actual SSE chunks, including tool_calls.
            # Do not buffer the completion, implement another model, retry a
            # POST or accept an endpoint/key from the incoming request.
            provider = current['provider']
            url = urlsplit(provider['url'])
            deadline = min(current['end'], time.monotonic()+20)
            conn = self.connection_factory(url.hostname,443,timeout=min(10,deadline-time.monotonic()))
            try:
                self.authorize(auth)
                conn.request('POST',url.path,raw,{'Authorization':'Bearer '+provider['api_key'],'Content-Type':'application/json'})
                response = conn.getresponse()
                if response.status != 200 or 'text/event-stream' not in response.headers.get('Content-Type','').lower():
                    raise RuntimeError('provider callback did not stream')
                self.authorize(auth)
                self.headers(handler);sent = True
                total = chunks = 0
                first_data_ms = None
                while True:
                    self.authorize(auth)
                    remaining = deadline-time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError('callback deadline')
                    if conn.sock is not None:
                        conn.sock.settimeout(min(10,remaining))
                    part = response.read1(8192)
                    if not part:
                        break
                    if first_data_ms is None:
                        first_data_ms = round((time.monotonic()-started)*1000)
                    total += len(part);chunks += 1
                    if total > 1048576:
                        raise ValueError('oversized provider stream')
                    handler.wfile.write(part);handler.wfile.flush()
                print('R1_SCENE_SPEECH '+json.dumps(dict(mode='provider',bytes=total,chunks=chunks,first_data_ms=first_data_ms,
                      elapsed_ms=round((time.monotonic()-started)*1000))),flush=True)
            finally:
                conn.close()
        except PermissionError:
            if not sent:handler.reply(401,{'error':'inactive scene speech credential'})
        except (ValueError,TypeError):
            if not sent:handler.reply(400,{'error':'invalid scene speech request'})
        except Exception:
            if not sent:handler.reply(502,{'error':'scene speech callback unavailable'})
        finally:
            handler.close_connection = True

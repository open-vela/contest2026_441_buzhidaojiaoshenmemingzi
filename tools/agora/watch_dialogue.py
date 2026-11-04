"""Explicit read-only live history inspection. No join, audio or file output.

Agora history is available only while the instance is running. Reuse the
existing authenticated REST client; print user/assistant text only, never
system/tool messages, headers, room, credentials or the complete response.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import re
import sys
import time

import session_control as control


def dialogue_messages(response):
    if not isinstance(response, dict): raise ValueError('invalid history')
    data = response.get('data', response)
    if not isinstance(data, dict): raise ValueError('invalid history data')
    contents = data.get('contents')
    if not isinstance(contents, list) or len(contents) > 128:
        raise ValueError('invalid history contents')
    return [{'role': item['role'], 'text': item['content'][:2000]}
            for item in contents if isinstance(item, dict) and
            item.get('role') in ('user', 'assistant') and
            isinstance(item.get('content'), str) and item['content'].strip()]


def joined_agent(events, after_unix_ms=0):
    """Select one authorized run without reopening an older daily session."""
    for event in events:
        if not isinstance(event, dict): continue
        value, timestamp = event.get('agent_id'), event.get('unix_ms')
        if after_unix_ms and (type(timestamp) is not int or timestamp <= after_unix_ms):
            continue
        if (event.get('event') == 'joined' and isinstance(value, str) and
                re.fullmatch(r'[A-Za-z0-9_-]{16,128}', value)):
            return value
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--controller-log', type=Path, required=True)
    parser.add_argument('--duration', type=int, default=660)
    parser.add_argument('--agent-seconds', type=int, default=75,
                        help='bounded read window for one authorized instance, up to195s')
    parser.add_argument('--after-unix-ms', type=int, default=0,
                        help='inspect a join after this timestamp; does not limit the voice session')
    args = parser.parse_args()
    if (not args.controller_log.is_absolute() or not 1 <= args.duration <= 660 or
            not 15 <= args.agent_seconds <= 195 or args.after_unix_ms < 0):
        parser.error('absolute task controller log and duration1..660 required')
    if hasattr(sys.stdout, 'reconfigure'): sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    deadline = time.monotonic() + args.duration
    agent = None
    seen = set()
    read_ok = False
    print('READONLY_HISTORY_READY: waiting for one authorized live instance; no file recording', flush=True)
    while time.monotonic() < deadline:
        raw = args.controller_log.read_text(encoding='utf-8', errors='replace') if args.controller_log.exists() else ''
        events = []
        for line in raw.splitlines():
            if line.startswith('R1_SESSION '):
                try: events.append(json.loads(line[len('R1_SESSION '):]))
                except ValueError: pass
        if agent is None:
            agent = joined_agent(events, args.after_unix_ms)
            if agent is not None:
                deadline = min(deadline, time.monotonic() + args.agent_seconds)
        if agent is None:
            time.sleep(0.2)
            continue
        if any(e.get('agent_id') == agent and e.get('event') in ('leave_acknowledged', 'leave_absent') for e in events):
            break
        try:
            with redirect_stdout(io.StringIO()):
                response = control.request(control.account(), '/agents/' + agent + '/history')
            for message in dialogue_messages(response):
                key = (message['role'], message['text'])
                if key not in seen:
                    print('LIVE_DIALOG ' + json.dumps(message, ensure_ascii=False), flush=True)
                    seen.add(key)
            read_ok = True
        except RuntimeError:
            # A just-created agent can be temporarily absent; confirmed
            # leave in the controller log, or this bounded deadline, stops us.
            print('LIVE_HISTORY_UNAVAILABLE: REST read failed; no join/retry of voice', flush=True)
        except ValueError:
            print('LIVE_HISTORY_SCHEMA_UNSUPPORTED: raw response suppressed', flush=True)
            break
        time.sleep(3)
    print('LIVE_HISTORY_DONE ' + json.dumps({'read_succeeded': read_ok, 'distinct_messages': len(seen)}), flush=True)


if __name__ == '__main__': main()

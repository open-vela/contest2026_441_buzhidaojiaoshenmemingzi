"""Explicit, bounded model-policy evaluation with synthetic scene results.

No RTC join, microphone, board command or real photograph. The real deployed
LLM is called only with --run; this does not validate camera/hardware quality.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
from contextlib import redirect_stdout
import copy
import io
import hashlib
import json
from pathlib import Path
import re
import urllib.error
import urllib.request
from session_server import Manager
import session_control as control

CASES = (
    dict(name='hospital-registration', context='我已到医院，想找挂号窗口。',
         goal=('医院', '挂号'), relevance=('挂号',), uncertain=False,
         observation='画面右侧服务窗口上方标牌能读到“挂号”，左侧有一扇门。'),
    dict(name='station-target-absent', context='我在车站，想找候车标识。',
         goal=('车站', '候车'), relevance=('候车', '标识'), uncertain=True,
         observation='镜头只拍到桌子和电脑，没有看到站名或候车标识。'),
    dict(name='classroom-blurred-sign', context='我到教室门口了，要找A301门牌。',
         goal=('教室', 'A301'), relevance=('门牌', 'A301'), uncertain=True,
         observation='画面中间有一块门牌，但文字模糊，无法确认房间号码。'),
)


def deployed_policy():
    """Capture the real Manager's request at a mocked cloud boundary."""
    bodies = []
    def mock_api(cfg, path, method='GET', body=None):
        if path == '/join':
            bodies.append(body)
            return {'agent_id': 'synthetic_policy_fixture'}
        return {}
    manager = Manager({'AGORA_APP_ID': 'a'*32}, 'fixture', api=mock_api,
                      mint=lambda *args: 'fixture_token', allow_paid_pipeline=True,
                      scene_tool_url='https://example.invalid/vision/tool')
    with redirect_stdout(io.StringIO()):
        session = manager.start(vision_capable=True)
        manager.stop(session['lease'])
    llm = bodies[0]['properties']['llm']
    tools = [{'type': item['type'], 'function': item['function']} for item in llm['tools']]
    return llm, tools


def evaluate(cases=CASES, trace_fixtures=False):
    cfg, tools = deployed_policy()
    requests = 0
    def ask(messages):
        nonlocal requests
        if requests >= len(cases)*3: raise RuntimeError('evaluation request budget exhausted')
        requests += 1
        payload = {**cfg['params'], 'stream': False,
                   'messages': messages, 'tools': tools, 'tool_choice': 'auto'}
        request = urllib.request.Request(cfg['url'], data=json.dumps(payload).encode(),
                    headers={'Authorization': 'Bearer '+cfg['api_key'], 'Content-Type': 'application/json'})
        try:
            with urllib.request.urlopen(request, timeout=25) as response:
                data = response.read(65537)
                if len(data)>65536: raise ValueError('oversized model response')
                result = json.loads(data)
        except urllib.error.HTTPError as error:
            raise RuntimeError('model HTTP '+str(error.code)) from None
        return result['choices'][0]['message']
    def question(message):
        calls = message.get('tool_calls') or []
        if len(calls)!=1 or calls[0]['function']['name']!='observeScene':
            raise ValueError('expected exactly one new observation request')
        args = json.loads(calls[0]['function']['arguments'])
        value = args.get('question')
        if not isinstance(value,str) or len(value.encode())>240:
            raise ValueError('invalid scene question')
        return value, calls[0]['id']
    results = []
    for case in cases:
        messages = copy.deepcopy(cfg['system_messages']) + [
            {'role':'user','content':case['context']},
            {'role':'assistant','content':'好的，我会留意与这个目标有关的标识。'},
            {'role':'user','content':'帮我看看前面有什么。'}]
        first = ask(messages)
        first_question, call = question(first)
        messages += [first, {'role':'tool','tool_call_id':call,'content':json.dumps({
            'ok':True,'navigation':False,'text':case['observation']}, ensure_ascii=False)}]
        answer = ask(messages)
        text = answer.get('content') or ''
        if not isinstance(text,str): raise ValueError('non-text answer')
        messages += [answer, {'role':'user','content':'现在换个方向，再看一眼。'}]
        second_question, _ = question(ask(messages))
        if trace_fixtures:
            print('SYNTHETIC_POLICY_TEXT '+json.dumps(dict(case=case['name'],
                first_question=first_question, answer=text, repeat_question=second_question),ensure_ascii=False))
        item = dict(name=case['name'], first_goal=all(g in first_question for g in case['goal']),
                    repeat_goal=all(g in second_question for g in case['goal']),
                    relevant=any(g in text for g in case['relevance']),
                    verbatim_observation=text.strip()==case['observation'],
                    uncertainty=(not case['uncertain'] or bool(re.search('没|未|无法|不清|看不出',text))),
                    no_navigation=not bool(re.search('左转|右转|直走|靠近|往前走|向前走|前进|后退|转身|走近|走[一二三四五六七八九十0-9]+步|[0-9]+米',text)),
                    no_tool_loop=not bool(answer.get('tool_calls')))
        item['passed'] = all(value for key,value in item.items() if key not in ('name','passed'))
        results.append(item)
    return dict(evaluation_version=2, model=cfg['params']['model'], temperature=cfg['params'].get('temperature'),
                max_tokens=cfg['params']['max_tokens'],
                policy_sha256=hashlib.sha256(json.dumps(cfg['system_messages'],ensure_ascii=False).encode()).hexdigest(),
                requests=requests, synthetic_scenes=True,
                rtc_joins=0, hardware_acceptance=False, passed=all(r['passed'] for r in results), cases=results)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run',action='store_true',help='call the configured LLM exactly up to9 times; may charge')
    parser.add_argument('--case',choices=[case['name'] for case in CASES],help='evaluate one synthetic case only (up to3 requests)')
    parser.add_argument('--trace-fixtures',action='store_true',help='print only the synthetic fixture questions/answer; never live dialogue')
    parser.add_argument('--output',type=Path,help='absolute metadata-only evidence path; must not already exist')
    args=parser.parse_args()
    if not args.run:
        print('DRY_RUN:3 synthetic scene cases, at most9 LLM requests; no network or board operation')
        return
    if args.output and (not args.output.is_absolute() or args.output.exists() or not args.output.parent.is_dir()):
        parser.error('output must be a new absolute path in an existing directory')
    cases=tuple(case for case in CASES if not args.case or case['name']==args.case)
    result=evaluate(cases,args.trace_fixtures)
    if args.output: args.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False))
    if not result['passed']: raise SystemExit(1)


if __name__=='__main__':
    main()

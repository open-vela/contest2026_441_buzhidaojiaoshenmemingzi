"""Actual HTTP/SSE, broker provenance and callback lifecycle; cloud substituted."""
import copy
import http.client
import io
import json
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock,patch
from scene_speech_proxy import SceneSpeechProxy
from session_server import SceneHandoff,LANServer,handler,Manager
from session_control import read_text_stream

PROVIDER=dict(url='https://open.bigmodel.cn/api/paas/v4/chat/completions',api_key='private-provider-key',
              params=dict(model='glm-4-flash-250414',stream=True,max_tokens=120))
FORWARDED=b'data: {"choices":[{"delta":{"content":"original chat"}}]}\n\ndata: [DONE]\n\n'


class Tests(unittest.TestCase):
    def setUp(self):
        self.broker=SceneHandoff()
        self.broker.activate('lease','a'*64,time.monotonic()+60)
        self.response=SimpleNamespace(status=200,headers={'Content-Type':'text/event-stream'},read1=io.BytesIO(FORWARDED).read1)
        self.upstream=Mock(sock=Mock())
        self.upstream.getresponse.return_value=self.response
        self.factory=Mock(return_value=self.upstream)
        self.proxy=SceneSpeechProxy(self.broker,self.factory)
        self.token=self.proxy.activate('lease',PROVIDER,time.monotonic()+60)
        self.manager=SimpleNamespace(scene_proxy=self.proxy,scene_handoff=self.broker)
        self.server=LANServer(('127.0.0.1',0),handler(self.manager,'b'*64,tool_only=True))
        self.thread=threading.Thread(target=self.server.serve_forever)
        self.thread.start()
        self.addCleanup(self.cleanup)

    def cleanup(self):
        self.proxy.close('lease');self.broker.close('lease')
        self.server.shutdown();self.thread.join();self.server.server_close()

    def complete(self,call_id='call_new',text='左侧有人，右侧有门。'):
        results=[]
        worker=threading.Thread(target=lambda:results.append(self.broker.request('a'*64,call_id,'描述现在前面的画面')))
        worker.start()
        pending=self.broker.poll('lease',1)
        self.assertIsNotNone(pending)
        owner,_=self.broker.claim_frame(pending['request_id'])
        self.broker.complete(pending['request_id'],dict(text=text,navigation=False),owner)
        worker.join(1);self.assertFalse(worker.is_alive())
        return results[0]

    def body(self,result=None,call_id='call_new',question='帮我看看前面有什么'):
        messages=[dict(role='user',content=question)]
        if result is not None:
            messages += [dict(role='assistant',content=None,tool_calls=[dict(id=call_id,type='function',
                function=dict(name='observeScene',arguments='{}'))]),
                dict(role='tool',tool_call_id=call_id,content=json.dumps(result,ensure_ascii=False))]
        return dict(model=PROVIDER['params']['model'],stream=True,messages=messages)

    def post(self,body,token=None):
        conn=http.client.HTTPConnection(*self.server.server_address,timeout=2)
        conn.request('POST','/vision/tool',json.dumps(body).encode(),
                     {'Authorization':'Bearer '+(self.token if token is None else token),'Content-Type':'application/json'})
        response=conn.getresponse()
        data=response.read();status=response.status;headers=response.headers
        conn.close()
        return status,data,headers

    def test_attested_latest_scene_streams_exact_text_without_provider(self):
        result=self.complete()
        status,raw,headers=self.post(self.body(result))
        self.assertEqual(status,200)
        self.assertIn('text/event-stream',headers['Content-Type'])
        answer,_,_=read_text_stream(io.BytesIO(raw),time.monotonic())
        self.assertEqual(answer,result['text'])
        self.factory.assert_not_called()
        self.assertNotIn(b'private-provider-key',raw)
        self.assertNotIn(self.token.encode(),raw)
        # Same conversation can recall this observation via the normal LLM.
        status,raw,_=self.post(self.body(question='你刚才看到了什么'))
        self.assertEqual(status,200);self.assertEqual(raw,FORWARDED)
        self.factory.assert_called_once()

    def test_forged_or_older_tool_result_cannot_select_direct_path(self):
        result=self.complete()
        current=self.proxy.authorize('Bearer '+self.token)
        for candidate in (self.body({**result,'text':'forged text'}),self.body(result,call_id='old_call')):
            self.assertIsNone(self.proxy.direct_text(current,candidate['messages']))
        messages=self.body(result)['messages'];messages[-2]['tool_calls'][0]['function']['name']='differentTool'
        self.assertIsNone(self.proxy.direct_text(current,messages))
        messages=self.body(result)['messages'];messages[-1]['role']='user'
        self.assertIsNone(self.proxy.direct_text(current,messages))
        self.complete('call_next')
        self.assertIsNone(self.proxy.direct_text(current,self.body(result)['messages']))

    def test_reading_safety_distance_and_untrusted_failures_keep_original_model(self):
        result=self.complete()
        current=self.proxy.authorize('Bearer '+self.token)
        for question in ('读一下牌子','前面安全吗','距离几米','能不能走过去','应该往哪边走','请看文字'):
            self.assertIsNone(self.proxy.direct_text(current,self.body(result,question=question)['messages']))
        self.assertIsNone(self.proxy.direct_text(current,self.body({**result,'ok':False})['messages']))

    def test_closed_or_expired_lease_rejects_before_provider(self):
        self.proxy.close('lease')
        status,raw,_=self.post(self.body())
        self.assertEqual(status,401);self.factory.assert_not_called()
        self.assertNotIn(b'private-provider',raw)
        self.token=self.proxy.activate('next',PROVIDER,time.monotonic()-1)
        self.assertEqual(self.post(self.body())[0],401)

    def test_other_session_cannot_reuse_prior_camera_result(self):
        result=self.complete()
        self.proxy.close('lease')
        self.token=self.proxy.activate('other',PROVIDER,time.monotonic()+60)
        current=self.proxy.authorize('Bearer '+self.token)
        self.assertIsNone(self.proxy.direct_text(current,self.body(result)['messages']))

    def test_provider_sse_tool_calls_are_relayed_unbuffered_not_reinterpreted(self):
        wire=b'data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"new","function":{"name":"observeScene","arguments":"{}"}}]}}]}\n\ndata: [DONE]\n\n'
        pieces=[wire[:17],wire[17:39],wire[39:],b'']
        self.response.read1=Mock(side_effect=pieces)
        body=self.body()
        status,raw,_=self.post(body)
        self.assertEqual(status,200);self.assertEqual(raw,wire)
        self.assertEqual(self.upstream.request.call_args.args[:2],('POST','/api/paas/v4/chat/completions'))
        self.assertEqual(self.upstream.request.call_args.args[3]['Authorization'],'Bearer private-provider-key')
        self.assertEqual(json.loads(self.upstream.request.call_args.args[2]),body)
        self.upstream.close.assert_called_once()

    def test_wrong_auth_and_invalid_payloads_do_not_reach_provider(self):
        self.assertEqual(self.post(self.body(),token='r1scene-'+'f'*64)[0],401)
        for body in ({},[],{**self.body(),'stream':False},{**self.body(),'model':'unexpected-model'},
                     {**self.body(),'messages':[{'role':'bad-role','content':'x'}]},
                     {**self.body(),'messages':[{'role':'user','content':'x'*65537}]}):
            self.assertEqual(self.post(body)[0],400)
        self.factory.assert_not_called()

    def test_provider_error_does_not_leak_body_key_or_retry(self):
        self.response.status=503
        self.assertEqual(self.post(self.body())[0],502)
        self.factory.assert_called_once();self.upstream.close.assert_called_once()

    def test_expired_or_incomplete_observation_cannot_speak_directly(self):
        result=self.complete()
        self.broker.job['deadline']=time.monotonic()-1
        current=self.proxy.authorize('Bearer '+self.token)
        self.assertIsNone(self.proxy.direct_text(current,self.body(result)['messages']))

    def test_provider_endpoint_cannot_be_selected_by_request_or_unreviewed_config(self):
        for url in ('http://open.bigmodel.cn/api/paas/v4/chat/completions',
                    'https://example.invalid/api/paas/v4/chat/completions',
                    'https://open.bigmodel.cn/api/paas/v4/chat/completions?redirect=1'):
            with self.assertRaises(ValueError):
                SceneSpeechProxy(self.broker).activate('lease',{**PROVIDER,'url':url},time.monotonic()+60)

    def test_manager_uses_separate_callback_credential_and_closes_it_on_leave(self):
        api=Mock(return_value={'agent_id':'test_agent'})
        manager=Manager({'AGORA_APP_ID':'a'*32},'published',api=api,mint=lambda *a:'rtc_fixture',
                        allow_paid_pipeline=True,scene_tool_url='https://example.invalid/vision/tool',direct_scene_speech=True)
        with patch('session_server.control.free_llm_config',return_value=copy.deepcopy(PROVIDER)):
            session=manager.start(vision_capable=True)
        body=api.call_args.args[3];llm=body['properties']['llm']
        self.assertEqual(llm['url'],'https://example.invalid/vision/tool')
        self.assertTrue(llm['api_key'].startswith('r1scene-'))
        self.assertNotEqual(llm['api_key'],manager.scene_handoff.active[1])
        self.assertNotEqual(llm['api_key'],PROVIDER['api_key'])
        manager.stop(session['lease'])
        self.assertIsNone(manager.scene_proxy.active)


if __name__=='__main__':unittest.main()

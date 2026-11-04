"""Offline rehearsal lifecycle: manual joins only; default paid cap unchanged."""
from contextlib import redirect_stdout,redirect_stderr
import io
import unittest
from unittest.mock import patch
from session_server import Manager,main

class DemoProfileTests(unittest.TestCase):
    def make(self,**changes):
        self.calls=[]
        def api(cfg,path,method='GET',body=None):
            self.calls.append(path)
            return {'agent_id':'demo_fixture_'+str(len(self.calls))} if path=='/join' else {}
        args=dict(cfg={'AGORA_APP_ID':'a'*32},pipeline_id='fixture',api=api,
            mint=lambda *a:'fixture-token',allow_paid_pipeline=True,
            scene_tool_url='https://preflight.invalid/vision/tool',session_seconds=300,demo_sessions=3)
        args.update(changes)
        return Manager(**args)

    def test_three_explicit_sessions_and_fresh_tool_owners(self):
        manager=self.make();self.assertEqual(self.calls,[])
        self.assertEqual(manager.max_starts*manager.session_seconds,900)
        leases=set()
        with redirect_stdout(io.StringIO()),patch('session_server.control.free_llm_config',return_value={'params':{'model':'fixture'}}):
            for index in range(3):
                session=manager.start(vision_capable=True);leases.add(session['lease'])
                self.assertIsNone(manager.scene_handoff.job)
                self.assertEqual(self.calls.count('/join'),index+1)
                manager.stop(session['lease']);manager.reap()
                self.assertEqual(self.calls.count('/join'),index+1)  # No automatic next join.
            with self.assertRaises(RuntimeError):manager.start(vision_capable=True)
        self.assertEqual(len(leases),3)

    def test_bounds_and_default_diagnostic_unchanged(self):
        for changes in ({'demo_sessions':0},{'demo_sessions':4},{'demo_sessions':True},
                        {'session_seconds':301},{'allow_paid_pipeline':False},
                        {'scene_tool_url':None},{'demo_sessions':None,'session_seconds':61}):
            with self.subTest(changes=changes),self.assertRaises(ValueError):self.make(**changes)
        manager=self.make(demo_sessions=None,session_seconds=60)
        self.assertEqual(manager.max_starts,1)

    def test_ambiguous_failed_join_consumes_one_slot_without_retry(self):
        manager=self.make(demo_sessions=1)
        def fail(*a,**kw):raise RuntimeError('join ambiguous')
        manager.api=fail
        with patch('session_server.control.free_llm_config',return_value={'params':{'model':'fixture'}}):
            with self.assertRaises(RuntimeError):manager.start(vision_capable=True)
            self.assertEqual(manager.starts,1)
            with self.assertRaises(RuntimeError):manager.start(vision_capable=True)
        self.assertEqual(manager.sessions,{})

    def test_invalid_cli_profiles_fail_before_account_or_network(self):
        for extra in (['--demo-sessions','3'],
                      ['--demo-sessions','4','--allow-paid-pipeline','--vision','--vision-tool-url','https://example.invalid/vision/tool'],
                      ['--demo-sessions','3','--allow-paid-pipeline','--vision','--vision-tool-url','https://example.invalid/vision/tool','--duration','0']):
            with patch('sys.argv',['session_server.py',*extra]),patch('session_server.control.account') as account,redirect_stderr(io.StringIO()),self.assertRaises(SystemExit):
                main()
            account.assert_not_called()

"""Policy evaluator's opt-in and real deployment contract; no model calls."""
from contextlib import redirect_stdout
import io
import json
import unittest
from unittest.mock import patch
import eval_scene_policy as evaluation


class Tests(unittest.TestCase):
    def test_default_dry_run_never_reads_config_or_calls_network(self):
        with patch('sys.argv', ['eval_scene_policy.py']), \
             patch.object(evaluation, 'deployed_policy') as policy, \
             patch('eval_scene_policy.urllib.request.urlopen') as network, \
             redirect_stdout(io.StringIO()) as output:
            evaluation.main()
        policy.assert_not_called(); network.assert_not_called()
        self.assertIn('DRY_RUN', output.getvalue())

    def test_schema_capture_uses_real_manager_without_real_rtc_or_token(self):
        config=dict(params=dict(model='fixture',stream=True,max_tokens=120),
                    system_messages=[],url='https://example.invalid/chat/completions',api_key='fixture-key')
        with patch('session_server.control.free_llm_config', return_value=config), \
             patch('session_server.control.request') as cloud, \
             patch('session_server.control.token') as token:
            deployed, tools = evaluation.deployed_policy()
        cloud.assert_not_called(); token.assert_not_called()
        self.assertEqual(deployed['params']['temperature'], 0.2)
        self.assertEqual(tools[0]['function']['name'], 'observeScene')
        self.assertNotIn('server', tools[0])
        self.assertIn('视障', deployed['system_messages'][0]['content'])

    def test_evaluation_preserves_production_token_budget_and_no_hardware_claim(self):
        config=dict(params=dict(model='fixture',stream=True,max_tokens=120,temperature=0.2),
                    system_messages=[dict(role='system',content='fixture')],
                    url='https://example.invalid/chat/completions',api_key='fixture-key')
        responses=[]
        for index, case in enumerate(evaluation.CASES):
            def tool(suffix):
                return dict(role='assistant',content=None,tool_calls=[dict(id=f'call{index}{suffix}',type='function',
                    function=dict(name='observeScene',arguments=json.dumps(dict(question='；'.join(case['goal'])))))])
            for message in (tool('a'),dict(role='assistant',content=case['observation']),tool('b')):
                responses.append(io.BytesIO(json.dumps(dict(choices=[dict(message=message)])).encode()))
        with patch.object(evaluation,'deployed_policy',return_value=(config,[])), \
             patch('eval_scene_policy.urllib.request.urlopen',side_effect=responses) as network:
            result=evaluation.evaluate()
        self.assertEqual(network.call_count,9)
        for call in network.call_args_list:
            body=json.loads(call.args[0].data)
            self.assertEqual(body['max_tokens'],120)
            self.assertEqual(body['temperature'],0.2)
            self.assertFalse(body['stream'])
        self.assertTrue(result['passed'])
        self.assertEqual(result['rtc_joins'],0)
        self.assertFalse(result['hardware_acceptance'])
        self.assertTrue(result['synthetic_scenes'])

    def test_blurred_sign_with_added_movement_advice_is_a_failure(self):
        case=evaluation.CASES[2]
        config=dict(params=dict(model='fixture',stream=True,max_tokens=120),system_messages=[],
                    url='https://example.invalid/chat/completions',api_key='fixture-key')
        tool=dict(role='assistant',tool_calls=[dict(id='fixture',type='function',
                  function=dict(name='observeScene',arguments=json.dumps(dict(question='教室A301门牌'))))])
        messages=(tool,dict(role='assistant',content='门牌看不清，建议靠近一点查看。'),tool)
        responses=[io.BytesIO(json.dumps(dict(choices=[dict(message=m)])).encode()) for m in messages]
        with patch.object(evaluation,'deployed_policy',return_value=(config,[])), \
             patch('eval_scene_policy.urllib.request.urlopen',side_effect=responses):
            result=evaluation.evaluate((case,))
        self.assertFalse(result['passed'])
        self.assertFalse(result['cases'][0]['no_navigation'])
        self.assertFalse(result['cases'][0]['verbatim_observation'])


if __name__=='__main__':
    unittest.main()

"""Offline history filter: never expose full cloud response or secrets."""
import unittest
from watch_dialogue import dialogue_messages, joined_agent


class DialogueTests(unittest.TestCase):
    def test_daily_log_cutoff_excludes_previous_session(self):
        old, new = 'A' * 32, 'B' * 32
        events = [{'event': 'joined', 'agent_id': old, 'unix_ms': 100},
                  {'event': 'leave_acknowledged', 'agent_id': old, 'unix_ms': 150},
                  {'event': 'joined', 'agent_id': new, 'unix_ms': 200}]
        self.assertEqual(joined_agent(events), old)
        self.assertEqual(joined_agent(events, 100), new)
        self.assertIsNone(joined_agent(events, 200))

    def test_cutoff_rejects_missing_or_malformed_join_metadata(self):
        events = [None, [], {'event': 'joined', 'agent_id': 'C' * 32},
                  {'event': 'joined', 'agent_id': 'D' * 32, 'unix_ms': '300'},
                  {'event': 'joined', 'agent_id': 'secret/path', 'unix_ms': 300}]
        self.assertIsNone(joined_agent(events, 100))

    def test_only_user_assistant_text_is_selected(self):
        payload = {'data': {'agent_id': 'private_instance', 'api_key': 'secret', 'contents': [
            {'role': 'system', 'content': 'system secret'},
            {'role': 'user', 'content': '帮我看看。'},
            {'role': 'assistant', 'content': '正在观察。', 'token': 'secret'},
            {'role': 'tool', 'content': 'tool internal'},
            {'role': 'assistant', 'content': None}, {'role': 'assistant', 'content': ' '}]}}
        self.assertEqual(dialogue_messages(payload), [
            {'role': 'user', 'text': '帮我看看。'}, {'role': 'assistant', 'text': '正在观察。'}])

    def test_unknown_schema_is_rejected_and_text_is_bounded(self):
        for payload in (None, [], {}, {'data': []}, {'contents': 'secret'}, {'contents': [{}] * 129}):
            with self.assertRaises(ValueError): dialogue_messages(payload)
        self.assertEqual(len(dialogue_messages({'contents': [{'role': 'user', 'content': '字' * 3000}]})[0]['text']), 2000)


if __name__ == '__main__': unittest.main()

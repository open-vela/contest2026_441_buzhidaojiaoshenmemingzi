"""Offline cloud diagnostics: no credentials, device, cloud or model calls."""
import json
import unittest
from unittest.mock import patch
from session_control import summarize_session, inspect_session


class InspectionTests(unittest.TestCase):
    def page(self):
        return {"total_turn_count": 2, "turns": [
            {"start": {"type": "greeting"}, "end": {"type": "ok", "metadata": {"secret": "hidden"}}},
            {"start": {"type": "silence_timeout"}, "end": {"type": "error", "metadata": {
                "reason": "LLM_REQUEST_ERR", "details": json.dumps({"error": {
                    "code": "1213", "message": "hidden transcript and key"}})}}}]}
    def test_only_counts_and_codes(self):
        result = summarize_session({"status": "STOPPED", "channel": "private"}, self.page())
        self.assertEqual(result["turn_types"]["greeting"], 1)
        self.assertEqual(result["turn_types"]["voice_input"], 0)
        self.assertEqual(result["provider_codes"], {"1213": 1})
        self.assertNotIn("hidden", json.dumps(result))
        self.assertNotIn("private", json.dumps(result))
    def test_unknown_status_or_malformed_page_rejected(self):
        for state, page in (({}, self.page()), ({"status": "unknown"}, self.page()),
                            ({"status": "RUNNING"}, {"total_turn_count": True, "turns": []}),
                            ({"status": "RUNNING"}, {"total_turn_count": 1, "turns": [None]})):
            with self.assertRaises(ValueError): summarize_session(state, page)
    def test_missing_prompt_count_does_not_expose_message(self):
        page = self.page()
        page["turns"][1]["end"]["metadata"]["details"] = {
            "error": {"code": "1213", "message": "missing prompt; PRIVATE_TEXT"}}
        result = summarize_session({"status": "STOPPED"}, page)
        self.assertEqual(result["missing_prompt_errors"], 1)
        self.assertNotIn("PRIVATE_TEXT", json.dumps(result))
        page["turns"][1]["end"]["metadata"]["details"]["error"]["message"] = None
        self.assertEqual(summarize_session({"status": "STOPPED"}, page)["missing_prompt_errors"], 0)
    def test_wrapped_response_and_unknown_reason_redacted(self):
        page = self.page(); page["turns"][1]["end"]["metadata"] = {"reason": "private-value"}
        result = summarize_session({"data": {"status": "RUNNING"}}, {"data": page})
        self.assertEqual(result["error_reasons"], {"OTHER": 1})
        self.assertNotIn("private-value", json.dumps(result))
    def test_only_two_gets_no_write(self):
        with patch("session_control.request", side_effect=[{"status": "STOPPED"}, self.page()]) as api:
            inspect_session({}, "a" * 32)
        self.assertEqual(api.call_count, 2)
        self.assertEqual(api.call_args_list[0].args[1], "/agents/" + "a" * 32)
        self.assertEqual(api.call_args_list[1].args[1], "/agents/" + "a" * 32 + "/turns?page_index=1&page_size=50")
        self.assertTrue(all(len(c.args) == 2 for c in api.call_args_list))
    def test_invalid_identifier_never_calls_api(self):
        with patch("session_control.request") as api:
            for ident in (None, "", "../join", "a" * 129, "a" * 16 + "?x=1"):
                with self.assertRaises(ValueError): inspect_session({}, ident)
            api.assert_not_called()


if __name__ == "__main__": unittest.main()

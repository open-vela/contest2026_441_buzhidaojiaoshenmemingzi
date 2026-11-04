# SPDX-License-Identifier: Apache-2.0
"""Review patched upstream request construction without API keys or calls.

Run: python test_mimo_adapter.py /path/to/patched/core/providers/llm/openai/openai.py
The real patched function is compiled from its AST; heavy server/model imports
are not mocked into a supposed end-to-end voice success.
"""

import ast
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from urllib.parse import urlparse


class Logger:
    def bind(self, **kwargs):
        return self

    def info(self, *args):
        pass


class AdapterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = ast.parse(Path(sys.argv[1]).read_text())
        provider = next(item for item in source.body
                        if isinstance(item, ast.ClassDef) and item.name == "LLMProvider")
        dictionary = next(item for item in source.body if isinstance(item, ast.Assign)
                          and any(isinstance(name, ast.Name) and name.id == "THINKING_DISABLED_DOMAINS"
                                  for name in item.targets))
        method = next(item for item in provider.body
                      if isinstance(item, ast.FunctionDef) and item.name == "_apply_thinking_disabled")
        namespace = {"urlparse": urlparse, "logger": Logger(), "TAG": "test",
                     "THINKING_DISABLED_DOMAINS": ast.literal_eval(dictionary.value)}
        exec(compile(ast.fix_missing_locations(ast.Module(body=[method], type_ignores=[])),
                     "reviewed-upstream-method", "exec"), namespace)
        cls.apply = staticmethod(namespace["_apply_thinking_disabled"])
        cls.provider = provider

    def test_mimo_stream_limit_and_thinking(self):
        params = {"stream": True, "max_tokens": 128}
        self.apply(SimpleNamespace(base_url="https://api.xiaomimimo.com/v1"), params)
        self.assertEqual(params, {"stream": True, "max_completion_tokens": 128,
                                  "extra_body": {"thinking": {"type": "disabled"}}})

    def test_other_provider_unchanged(self):
        params = {"stream": True, "max_tokens": 128}
        self.apply(SimpleNamespace(base_url="https://api.deepseek.com/v1"), params)
        self.assertEqual(params["max_tokens"], 128)
        self.assertNotIn("max_completion_tokens", params)

    def test_mimo_without_limit(self):
        params = {"stream": True}
        self.apply(SimpleNamespace(base_url="https://api.xiaomimimo.com/v1"), params)
        self.assertNotIn("max_completion_tokens", params)

    def test_both_response_paths_stream(self):
        for name in ("response", "response_with_functions"):
            method = next(item for item in self.provider.body
                          if isinstance(item, ast.FunctionDef) and item.name == name)
            payload = next(item.value for item in method.body if isinstance(item, ast.Assign)
                           and any(isinstance(target, ast.Name) and target.id == "request_params"
                                   for target in item.targets))
            fields = dict(zip((ast.literal_eval(key) for key in payload.keys), payload.values))
            self.assertTrue(ast.literal_eval(fields["stream"]))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)

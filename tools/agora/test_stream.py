"""Offline SSE checks: no paid API, microphone or speaker access."""
import io
import json
import time
import unittest
from session_control import read_text_stream


def event(content):
    return ("data: " + json.dumps({"choices": [{"delta": {"content": content}}]},
                                 ensure_ascii=False) + "\r\n\r\n").encode()


class StreamTests(unittest.TestCase):
    def parse(self, data):
        return read_text_stream(io.BytesIO(data), time.monotonic())
    def test_unicode_chunks_comments_usage_and_done(self):
        data = b": keepalive\r\n\r\n" + event(None) + event("你") + event("好")
        data += b'data: {"choices": [], "usage": {}}\n\ndata: [DONE]\n\n'
        text, first, chunks = self.parse(data)
        self.assertEqual((text, chunks), ("你好", 2))
        self.assertGreaterEqual(first, 0)
    def test_truncated_stream_is_not_success(self):
        with self.assertRaises(ValueError): self.parse(event("hello"))
    def test_empty_stream_is_not_success(self):
        with self.assertRaises(ValueError): self.parse(b"data: [DONE]\n\n")
    def test_error_or_wrong_content_is_not_success(self):
        for data in (b'data: {"error": "test"}\n\n', event(["invalid"]),
                     b'data: {"choices": "invalid"}\n\n',
                     b'data: {"choices": [null]}\n\n',
                     b'data: {"choices": [{"delta": null}]}\n\n'):
            with self.subTest(data=data), self.assertRaises(ValueError): self.parse(data)
    def test_line_size_is_bounded(self):
        with self.assertRaises(ValueError): self.parse(b"x" * 65537 + b"\n")
    def test_deadline_is_bounded(self):
        with self.assertRaises(TimeoutError):
            read_text_stream(io.BytesIO(event("hello")), time.monotonic() - 31)


if __name__ == "__main__": unittest.main()

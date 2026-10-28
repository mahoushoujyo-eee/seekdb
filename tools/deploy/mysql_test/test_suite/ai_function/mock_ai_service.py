#!/usr/bin/env python3
"""A mock AI service for the ai_function test suite.

The suite never talks to a real model, every request is answered by this
service. It speaks the protocols the ai functions use:

  * /v1/chat/completions  the openai completion protocol
  * /v1/embeddings        the openai embedding protocol
  * /v1/rerank            the rerank protocol
  * /api/generate         the ollama completion protocol
  * /api/chat             the ollama chat protocol
  * /api/embed            the ollama embedding protocol
  * /api/embeddings       the legacy ollama embedding protocol

Start it with

    python3 mock_ai_service.py --host 0.0.0.0 --port 8000

and the answers are deterministic, so the expected results of the test suite
do not change between the runs.
"""

import argparse
import json
import os
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# The dimension of the embeddings the service answers with.
EMBEDDING_DIMENSION = 4

# Set MOCK_AI_LOG to a file to keep a log of the requests that came in.
LOG_REQUESTS = os.environ.get("MOCK_AI_LOG")


def embedding_of(text):
    """The deterministic embedding of the given text."""
    raw = text.encode("utf-8")
    return [len(raw), zlib.crc32(raw) % 100, 0.5, 0.25]


def last_user_message(messages):
    content = ""
    for message in messages or []:
        if message.get("role") == "user":
            content = message.get("content", "")
    return content


class MockAIHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        # Keep the output of the test suite clean.
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b"{}"
        try:
            payload = json.loads(raw.decode("utf-8"))
        except ValueError:
            payload = {}
        if not isinstance(payload, dict):
            payload = {}
        if LOG_REQUESTS:
            with open(LOG_REQUESTS, "a", encoding="utf-8") as log:
                log.write("%s %s\n" % (self.path, json.dumps(payload)))
                log.flush()

        path = self.path.split("?")[0]
        if path == "/v1/chat/completions":
            body = self.chat_completions(payload)
        elif path == "/v1/embeddings":
            body = self.embeddings(payload)
        elif path == "/v1/rerank":
            body = self.rerank(payload)
        elif path == "/api/generate":
            body = self.generate(payload)
        elif path == "/api/chat":
            body = self.chat(payload)
        elif path == "/api/embed":
            body = self.embed(payload)
        elif path == "/api/embeddings":
            body = self.legacy_embed(payload)
        else:
            self.send_error(404)
            return

        data = json.dumps(body).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def chat_completions(self, payload):
        content = last_user_message(payload.get("messages"))
        return {
            "id": "chatcmpl-mock",
            "object": "chat.completion",
            "model": payload.get("model", "mock-model"),
            "choices": [
                {
                    "index": 0,
                    "message": {"role": "assistant", "content": "mock answer: " + content},
                    "finish_reason": "stop",
                }
            ],
            "usage": {"prompt_tokens": len(content), "completion_tokens": 1, "total_tokens": len(content) + 1},
        }

    def embeddings(self, payload):
        inputs = payload.get("input", [])
        if isinstance(inputs, str):
            inputs = [inputs]
        data = []
        for index, text in enumerate(inputs):
            data.append({"object": "embedding", "index": index, "embedding": embedding_of(text)})
        # A service is allowed to answer out of order, the sql layer has to
        # restore the order of the request.
        data.reverse()
        return {
            "object": "list",
            "data": data,
            "model": payload.get("model", "mock-model"),
            "usage": {"prompt_tokens": len(inputs), "total_tokens": len(inputs)},
        }

    def rerank(self, payload):
        query = payload.get("query", "")
        documents = payload.get("documents", [])
        results = []
        for index, document in enumerate(documents):
            if isinstance(document, str):
                text = document
            else:
                text = json.dumps(document, sort_keys=True)
            # The score grows with the number of characters the query and the
            # document have in common.
            score = round((len(set(query) & set(text)) + 1) / (len(query) + 1), 6)
            results.append({"index": index, "score": score})
        results.sort(key=lambda item: (-item["score"], item["index"]))
        return {
            "id": "rerank-mock",
            "results": results,
            "meta": {"tokens": {"input_tokens": len(query)}},
        }

    def generate(self, payload):
        prompt = payload.get("prompt", "")
        return {
            "model": payload.get("model", "mock-model"),
            "response": "mock answer: " + prompt,
            "done": True,
        }

    def chat(self, payload):
        content = last_user_message(payload.get("messages"))
        return {
            "model": payload.get("model", "mock-model"),
            "message": {"role": "assistant", "content": "mock answer: " + content},
            "done": True,
        }

    def embed(self, payload):
        inputs = payload.get("input", [])
        if isinstance(inputs, str):
            inputs = [inputs]
        return {
            "model": payload.get("model", "mock-model"),
            "embeddings": [embedding_of(text) for text in inputs],
        }

    def legacy_embed(self, payload):
        prompt = payload.get("prompt", "")
        return {
            "model": payload.get("model", "mock-model"),
            "embedding": embedding_of(prompt),
        }


def main():
    parser = argparse.ArgumentParser(description="mock ai service for the ai_function test suite")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000)
    arguments = parser.parse_args()
    server = ThreadingHTTPServer((arguments.host, arguments.port), MockAIHandler)
    print("mock ai service listening on %s:%d" % (arguments.host, arguments.port))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

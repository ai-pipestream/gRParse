"""An emulated OpenAI-compatible VLM for the chart-outputs end to end test.

Answers POST .../chat/completions the way a Granite Vision chart model does:
the <chart2csv>, <chart2summary> and <chart2code> prompts (or their natural
language forms) each get a fixed reply, so the test can check exactly what
lands in the Document. Every request is logged to stdout as one line naming
the model and which prompt arrived.
"""

import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CSV = "Region,Revenue\nNorth,120\nEast,95\nSouth,80\n"
SUMMARY = "Revenue is highest in the North and lowest in the South."
CODE = (
    "Here is the code:\n```python\nimport matplotlib.pyplot as plt\n"
    "plt.bar([\"North\", \"East\", \"South\"], [120, 95, 80])\nplt.show()\n```\n"
)


def prompt_text(body):
    parts = []
    for message in body.get("messages", []):
        content = message.get("content")
        if isinstance(content, str):
            parts.append(content)
        elif isinstance(content, list):
            parts.extend(p.get("text", "") for p in content if p.get("type") == "text")
    return "\n".join(parts)


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length) or b"{}")
        prompt = prompt_text(body)
        if "chart2summary" in prompt or "Describe this chart" in prompt:
            kind, content = "summary", SUMMARY
        elif "chart2code" in prompt or "code" in prompt.lower():
            kind, content = "code", CODE
        else:
            kind, content = "csv", CSV
        print(f"fake-vlm: model={body.get('model')} kind={kind} path={self.path}", flush=True)
        reply = json.dumps({
            "id": "fake", "object": "chat.completion", "model": body.get("model", ""),
            "choices": [{"index": 0, "finish_reason": "stop",
                         "message": {"role": "assistant", "content": content}}],
        }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()

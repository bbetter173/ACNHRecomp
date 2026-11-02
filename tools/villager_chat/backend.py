"""An opt-in HTTP backend for chat-completions-compatible model servers."""
import json
import urllib.parse
import urllib.request


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        return None


class HttpBackend:
    def __init__(self, endpoint: str, model: str, api_key: str | None = None):
        parsed = urllib.parse.urlsplit(endpoint)
        if parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
            raise ValueError("endpoint must be an explicit HTTP URL without credentials or query parameters")
        if parsed.scheme == "http" and parsed.hostname not in ("localhost", "127.0.0.1", "::1"):
            raise ValueError("remote endpoints require HTTPS")
        if not model.strip():
            raise ValueError("model must be configured")
        self.endpoint, self.model, self.api_key = endpoint, model, api_key
        self._opener = urllib.request.build_opener(_NoRedirect())

    def generate(self, messages: list[dict[str, str]], timeout: float) -> str:
        body = json.dumps({"model": self.model, "messages": messages, "max_tokens": 160, "stream": False}).encode("utf-8")
        headers = {"Content-Type": "application/json"}
        if self.api_key:
            headers["Authorization"] = "Bearer " + self.api_key
        request = urllib.request.Request(self.endpoint, data=body, headers=headers, method="POST")
        with self._opener.open(request, timeout=timeout) as response:
            raw = response.read(32769)
        if len(raw) > 32768:
            raise ValueError("model response exceeds its byte limit")
        value = json.loads(raw)
        choices = value["choices"]
        if not isinstance(choices, list) or len(choices) != 1:
            raise ValueError("model response must contain one choice")
        choice = choices[0]
        if choice.get("finish_reason") != "stop":
            raise ValueError("model response is incomplete")
        message = choice["message"]
        if message.get("tool_calls") or message.get("function_call") or message.get("role") != "assistant":
            raise ValueError("model response is not plain assistant dialogue")
        return message["content"]

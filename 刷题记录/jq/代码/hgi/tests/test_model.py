import json

from hgi.model import OpenAIResponsesBackend


def test_openai_input_conversion_preserves_tool_exchange():
    messages = [
        {"role": "user", "content": "inspect"},
        {
            "role": "assistant",
            "content": [
                {
                    "type": "tool_use",
                    "id": "call_1",
                    "name": "read_file",
                    "input": {"path": "a.py"},
                }
            ],
        },
        {
            "role": "user",
            "content": [
                {"type": "tool_result", "tool_use_id": "call_1", "content": "1: pass"}
            ],
        },
    ]

    items = OpenAIResponsesBackend._input_items(messages)
    assert items[1]["type"] == "function_call"
    assert json.loads(items[1]["arguments"]) == {"path": "a.py"}
    assert items[2] == {
        "type": "function_call_output",
        "call_id": "call_1",
        "output": "1: pass",
    }


def test_codex_config_loader_uses_provider_and_auth(tmp_path, monkeypatch):
    (tmp_path / "config.toml").write_text(
        'model_provider = "local"\n'
        '[model_providers.local]\n'
        'base_url = "http://localhost:1234/v1"\n'
        'wire_api = "responses"\n'
    )
    (tmp_path / "auth.json").write_text(json.dumps({"OPENAI_API_KEY": "test-key"}))

    class FakeClient:
        def __init__(self, **kwargs):
            self.kwargs = kwargs

    monkeypatch.setattr("openai.AsyncOpenAI", FakeClient)
    backend = OpenAIResponsesBackend.from_codex_config(
        codex_home=tmp_path, model="gpt-5.6-luna", reasoning_effort="low"
    )

    assert backend.client.kwargs == {
        "api_key": "test-key",
        "base_url": "http://localhost:1234/v1",
    }
    assert backend.model == "gpt-5.6-luna"
    assert backend.reasoning_effort == "low"

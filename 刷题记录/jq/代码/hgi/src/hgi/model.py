from __future__ import annotations

import json
import os
import tomllib
from pathlib import Path
from typing import Any, Protocol, Sequence

from .schemas import ModelReply, ModelUsage, ToolCall


class ModelBackend(Protocol):
    async def generate(
        self,
        *,
        system: str,
        messages: Sequence[dict[str, Any]],
        tools: Sequence[dict[str, Any]] = (),
    ) -> ModelReply: ...


class AnthropicBackend:
    """Thin adapter that keeps the rest of HGI independent of an SDK."""

    def __init__(self, model: str = "claude-sonnet-4-5", max_tokens: int = 4096):
        try:
            from anthropic import AsyncAnthropic
        except ImportError as exc:  # pragma: no cover - exercised by CLI install
            raise RuntimeError("Install the 'anthropic' package to use this backend") from exc
        self.client = AsyncAnthropic()
        self.model = model
        self.max_tokens = max_tokens

    async def generate(
        self,
        *,
        system: str,
        messages: Sequence[dict[str, Any]],
        tools: Sequence[dict[str, Any]] = (),
    ) -> ModelReply:
        kwargs: dict[str, Any] = {
            "model": self.model,
            "max_tokens": self.max_tokens,
            "system": system,
            "messages": list(messages),
        }
        if tools:
            kwargs["tools"] = list(tools)
        response = await self.client.messages.create(**kwargs)
        texts: list[str] = []
        calls: list[ToolCall] = []
        for block in response.content:
            if block.type == "text":
                texts.append(block.text)
            elif block.type == "tool_use":
                calls.append(ToolCall(id=block.id, name=block.name, arguments=block.input))
        usage = ModelUsage(
            input_tokens=getattr(response.usage, "input_tokens", 0),
            cached_input_tokens=getattr(response.usage, "cache_read_input_tokens", 0) or 0,
            output_tokens=getattr(response.usage, "output_tokens", 0),
            total_tokens=(
                getattr(response.usage, "input_tokens", 0)
                + getattr(response.usage, "output_tokens", 0)
            ),
        )
        return ModelReply(
            text="\n".join(texts),
            tool_calls=calls,
            usage=usage,
            response_id=getattr(response, "id", None),
        )


class OpenAIResponsesBackend:
    """Responses API adapter, including custom providers used by Codex."""

    def __init__(
        self,
        *,
        api_key: str,
        base_url: str | None = None,
        model: str = "gpt-5.6-luna",
        reasoning_effort: str = "low",
        max_output_tokens: int = 4096,
    ):
        try:
            from openai import AsyncOpenAI
        except ImportError as exc:  # pragma: no cover - exercised by CLI install
            raise RuntimeError("Install the 'openai' package to use this backend") from exc
        self.client = AsyncOpenAI(api_key=api_key, base_url=base_url)
        self.model = model
        self.reasoning_effort = reasoning_effort
        self.max_output_tokens = max_output_tokens

    @classmethod
    def from_codex_config(
        cls,
        *,
        model: str = "gpt-5.6-luna",
        reasoning_effort: str = "low",
        codex_home: Path | None = None,
        max_output_tokens: int = 4096,
    ) -> "OpenAIResponsesBackend":
        root = codex_home or Path(os.getenv("CODEX_HOME", Path.home() / ".codex"))
        config_path = root / "config.toml"
        auth_path = root / "auth.json"
        if not config_path.is_file() or not auth_path.is_file():
            raise RuntimeError(f"Codex config or authentication is missing below {root}")
        config = tomllib.loads(config_path.read_text(encoding="utf-8"))
        provider_id = config.get("model_provider")
        provider = config.get("model_providers", {}).get(provider_id, {})
        if not provider_id or not provider:
            raise RuntimeError("Codex model_provider is not configured")
        if provider.get("wire_api", "responses") != "responses":
            raise RuntimeError(f"Codex provider '{provider_id}' does not use the Responses API")
        auth = json.loads(auth_path.read_text(encoding="utf-8"))
        api_key = auth.get("OPENAI_API_KEY")
        if not api_key:
            raise RuntimeError("OPENAI_API_KEY is missing from Codex authentication")
        return cls(
            api_key=api_key,
            base_url=provider.get("base_url"),
            model=model,
            reasoning_effort=reasoning_effort,
            max_output_tokens=max_output_tokens,
        )

    async def generate(
        self,
        *,
        system: str,
        messages: Sequence[dict[str, Any]],
        tools: Sequence[dict[str, Any]] = (),
    ) -> ModelReply:
        response_tools = [
            {
                "type": "function",
                "name": tool["name"],
                "description": tool["description"],
                "parameters": tool["input_schema"],
            }
            for tool in tools
        ]
        kwargs: dict[str, Any] = {
            "model": self.model,
            "instructions": system,
            "input": self._input_items(messages),
            "reasoning": {"effort": self.reasoning_effort},
            "max_output_tokens": self.max_output_tokens,
            "store": False,
        }
        if response_tools:
            kwargs["tools"] = response_tools
        response = await self.client.responses.create(**kwargs)
        texts: list[str] = []
        calls: list[ToolCall] = []
        for item in response.output:
            if item.type == "message":
                for content in item.content:
                    if content.type == "output_text":
                        texts.append(content.text)
            elif item.type == "function_call":
                try:
                    arguments = json.loads(item.arguments)
                except (TypeError, json.JSONDecodeError):
                    arguments = {}
                calls.append(ToolCall(id=item.call_id, name=item.name, arguments=arguments))
        usage = self._usage(response)
        return ModelReply(
            text="\n".join(texts),
            tool_calls=calls,
            usage=usage,
            response_id=getattr(response, "id", None),
        )

    @staticmethod
    def _usage(response: Any) -> ModelUsage | None:
        raw = getattr(response, "usage", None)
        if raw is None:
            return None
        input_details = getattr(raw, "input_tokens_details", None)
        output_details = getattr(raw, "output_tokens_details", None)
        input_tokens = getattr(raw, "input_tokens", 0) or 0
        output_tokens = getattr(raw, "output_tokens", 0) or 0
        return ModelUsage(
            input_tokens=input_tokens,
            cached_input_tokens=getattr(input_details, "cached_tokens", 0) or 0,
            output_tokens=output_tokens,
            reasoning_tokens=getattr(output_details, "reasoning_tokens", 0) or 0,
            total_tokens=getattr(raw, "total_tokens", input_tokens + output_tokens) or 0,
        )

    @staticmethod
    def _input_items(messages: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
        items: list[dict[str, Any]] = []
        for message in messages:
            content = message["content"]
            if isinstance(content, str):
                items.append({"role": message["role"], "content": content})
                continue
            for block in content:
                if block["type"] == "text":
                    items.append({"role": message["role"], "content": block["text"]})
                elif block["type"] == "tool_use":
                    items.append(
                        {
                            "type": "function_call",
                            "call_id": block["id"],
                            "name": block["name"],
                            "arguments": json.dumps(block["input"], ensure_ascii=False),
                        }
                    )
                elif block["type"] == "tool_result":
                    items.append(
                        {
                            "type": "function_call_output",
                            "call_id": block["tool_use_id"],
                            "output": block["content"],
                        }
                    )
        return items

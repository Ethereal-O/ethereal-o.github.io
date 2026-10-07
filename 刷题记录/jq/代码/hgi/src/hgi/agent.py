from __future__ import annotations

import inspect
from collections.abc import Awaitable, Callable
from typing import Any

from .model import ModelBackend
from .schemas import ToolCall
from .tools import Dispatcher

ToolResultCallback = Callable[[ToolCall, str], None | Awaitable[None]]
ContinueCallback = Callable[[], bool]


class AgentLoop:
    def __init__(
        self,
        model: ModelBackend,
        dispatcher: Dispatcher,
        *,
        max_turns: int = 20,
        soft_turns: int | None = None,
        finalize_on_limit: bool = False,
        max_tool_result_chars: int | None = None,
    ):
        self.model = model
        self.dispatcher = dispatcher
        self.max_turns = max_turns
        self.soft_turns = soft_turns
        self.finalize_on_limit = finalize_on_limit
        self.max_tool_result_chars = max_tool_result_chars

    @staticmethod
    def _fit_tool_output(output: str, allowance: int) -> str:
        if len(output) <= allowance:
            return output
        if allowance <= 0:
            return ""
        marker = "\n...[truncated by cumulative tool context budget; retry with a narrower query]"
        if allowance <= len(marker) + 32:
            return output[:allowance]
        return output[: allowance - len(marker)] + marker

    async def run(
        self,
        *,
        system: str,
        prompt: str,
        on_tool_result: ToolResultCallback | None = None,
        continue_after_soft_limit: ContinueCallback | None = None,
    ) -> str:
        messages: list[dict[str, Any]] = [{"role": "user", "content": prompt}]
        tool_result_chars = 0
        for turn in range(self.max_turns):
            hard_limit = turn == self.max_turns - 1
            soft_limit = (
                self.soft_turns is not None
                and turn >= self.soft_turns - 1
                and continue_after_soft_limit is not None
                and not continue_after_soft_limit()
            )
            context_limit = (
                self.max_tool_result_chars is not None
                and tool_result_chars >= self.max_tool_result_chars
            )
            final_turn = self.finalize_on_limit and (hard_limit or soft_limit or context_limit)
            if final_turn:
                messages.append(
                    {
                        "role": "user",
                        "content": (
                            "The tool-use budget is exhausted. Do not call more tools; "
                            "finish now using the evidence already collected."
                        ),
                    }
                )
            reply = await self.model.generate(
                system=system,
                messages=messages,
                tools=() if final_turn else self.dispatcher.schemas,
            )
            messages.append({"role": "assistant", "content": reply.assistant_content()})
            if not reply.tool_calls:
                return reply.text
            if final_turn:
                raise RuntimeError("agent requested tools after its tool-use budget expired")
            dispatched: list[tuple[ToolCall, str]] = []
            for call in reply.tool_calls:
                output = await self.dispatcher.dispatch(call.name, call.arguments)
                if on_tool_result:
                    callback_result = on_tool_result(call, output)
                    if inspect.isawaitable(callback_result):
                        await callback_result
                dispatched.append((call, output))

            results = []
            for index, (call, output) in enumerate(dispatched):
                if self.max_tool_result_chars is not None:
                    remaining = max(self.max_tool_result_chars - tool_result_chars, 0)
                    remaining_results = len(dispatched) - index
                    output = self._fit_tool_output(output, remaining // remaining_results)
                    tool_result_chars += len(output)
                results.append(
                    {
                        "type": "tool_result",
                        "tool_use_id": call.id,
                        "content": output,
                    }
                )
            messages.append({"role": "user", "content": results})
        raise RuntimeError(f"agent exceeded {self.max_turns} tool turns")

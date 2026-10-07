import pytest

from hgi.agent import AgentLoop
from hgi.schemas import ModelReply, ToolCall
from hgi.tools import Dispatcher, Tool


@pytest.mark.asyncio
async def test_agent_loop_caps_cumulative_tool_result_context():
    class InspectingModel:
        def __init__(self):
            self.calls = 0

        async def generate(self, *, system, messages, tools=()):
            self.calls += 1
            if self.calls == 1:
                return ModelReply(
                    tool_calls=[ToolCall(id="large", name="large_result")]
                )
            tool_result = next(
                block["content"]
                for message in messages
                if isinstance(message["content"], list)
                for block in message["content"]
                if block["type"] == "tool_result"
            )
            assert tool_result == "x" * 12
            assert not tools
            return ModelReply(text="finished")

    model = InspectingModel()
    dispatcher = Dispatcher(
        [
            Tool(
                "large_result",
                "Return a large result.",
                {"type": "object", "properties": {}},
                lambda: "x" * 100,
            )
        ]
    )
    loop = AgentLoop(
        model,
        dispatcher,
        max_turns=2,
        finalize_on_limit=True,
        max_tool_result_chars=12,
    )

    assert await loop.run(system="test", prompt="inspect") == "finished"


@pytest.mark.asyncio
async def test_agent_loop_shares_tool_context_budget_across_parallel_results():
    class InspectingModel:
        def __init__(self):
            self.calls = 0

        async def generate(self, *, system, messages, tools=()):
            self.calls += 1
            if self.calls == 1:
                return ModelReply(
                    tool_calls=[
                        ToolCall(id="first", name="large_result"),
                        ToolCall(id="second", name="large_result"),
                    ]
                )
            results = [
                block["content"]
                for message in messages
                if isinstance(message["content"], list)
                for block in message["content"]
                if block["type"] == "tool_result"
            ]
            assert len(results) == 2
            assert all(results)
            assert sum(map(len, results)) <= 400
            assert all("truncated by cumulative tool context budget" in item for item in results)
            return ModelReply(text="finished")

    loop = AgentLoop(
        InspectingModel(),
        Dispatcher(
            [
                Tool(
                    "large_result",
                    "Return a large result.",
                    {"type": "object", "properties": {}},
                    lambda: "x" * 1_000,
                )
            ]
        ),
        max_turns=2,
        max_tool_result_chars=400,
    )

    assert await loop.run(system="test", prompt="inspect") == "finished"


@pytest.mark.asyncio
async def test_agent_loop_forces_final_answer_after_tool_context_is_exhausted():
    class ToolHungryModel:
        def __init__(self):
            self.tool_visibility = []

        async def generate(self, *, system, messages, tools=()):
            self.tool_visibility.append(bool(tools))
            if tools:
                return ModelReply(tool_calls=[ToolCall(id="large", name="large_result")])
            assert "tool-use budget is exhausted" in messages[-1]["content"]
            return ModelReply(text="finished from existing evidence")

    model = ToolHungryModel()
    loop = AgentLoop(
        model,
        Dispatcher(
            [
                Tool(
                    "large_result",
                    "Return a large result.",
                    {"type": "object", "properties": {}},
                    lambda: "x" * 1_000,
                )
            ]
        ),
        max_turns=10,
        finalize_on_limit=True,
        max_tool_result_chars=400,
    )

    assert await loop.run(system="test", prompt="inspect") == "finished from existing evidence"
    assert model.tool_visibility == [True, False]


@pytest.mark.asyncio
async def test_agent_loop_extends_soft_budget_only_while_evidence_grows():
    class ToolUntilFinalModel:
        def __init__(self):
            self.tool_visibility = []

        async def generate(self, *, system, messages, tools=()):
            self.tool_visibility.append(bool(tools))
            if tools:
                number = len(self.tool_visibility)
                return ModelReply(
                    tool_calls=[ToolCall(id=f"call-{number}", name="evidence")]
                )
            return ModelReply(text="finished")

    growth = iter((True, False))
    model = ToolUntilFinalModel()
    loop = AgentLoop(
        model,
        Dispatcher(
            [
                Tool(
                    "evidence",
                    "Return evidence.",
                    {"type": "object", "properties": {}},
                    lambda: "evidence",
                )
            ]
        ),
        soft_turns=3,
        max_turns=6,
        finalize_on_limit=True,
    )

    result = await loop.run(
        system="test",
        prompt="inspect",
        continue_after_soft_limit=lambda: next(growth),
    )

    assert result == "finished"
    assert model.tool_visibility == [True, True, True, False]

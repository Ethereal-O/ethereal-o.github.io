import asyncio
import json

import pytest

from hgi.config import HGIConfig, WritePolicy
from hgi.controller import Controller, _goal_similarity
from hgi.buffer import SpeculativeBuffer
from hgi.schemas import Artifact, Evidence, ModelReply, ModelUsage, ToolCall


class ScriptedModel:
    def __init__(self):
        self.explorer_started = asyncio.Event()

    async def generate(self, *, system, messages, tools=()):
        if "input-boundary judge" in system:
            return ModelReply(
                text='{"action":"explore","exploration_goal":"find auth code",'
                '"reason":"stable intent","confidence":0.9}'
            )
        if "Maintain a concise summary" in system:
            return ModelReply(text="Investigate authentication")
        if "read-only coding repository explorer" in system:
            self.explorer_started.set()
            return ModelReply(
                text='{"summary":"auth entry found","findings":['
                '{"path":"auth.py","line":1,"claim":"entry point"}],'
                '"unresolved":[],"confidence":0.8}'
            )
        if "committed coding agent" in system:
            assert "auth entry found" in messages[0]["content"]
            return ModelReply(text="Implemented using prior evidence")
        raise AssertionError(system)


def test_goal_similarity_uses_stable_identifiers_without_merging_unrelated_goals():
    permission_goal = (
        "审查 HGI 从 CLI 的 writes 参数到 write_file 和 run_command 的完整权限链路，"
        "说明 allow、ask、deny，并检查路径逃逸和 shell 注入防护。"
    )
    rephrased = (
        "只读分析 HGI 的 CLI writes 配置如何传递到 write_file、run_command，"
        "明确 allow、ask、deny，并核查 shell 注入及路径逃逸防护。"
    )
    unrelated = "分析 HGI 日志中 usage 的成本聚合、脱敏和文件轮转策略。"

    assert _goal_similarity(permission_goal, rephrased) >= 0.72
    assert _goal_similarity(permission_goal, unrelated) < 0.72


@pytest.mark.asyncio
async def test_typing_explores_and_commit_reuses_artifact(tmp_path):
    (tmp_path / "auth.py").write_text("login = True\n")
    model = ScriptedModel()
    config = HGIConfig(
        workspace=tmp_path,
        state_dir=tmp_path / ".state",
        pause_ms=100,
        min_judge_interval_ms=0,
    )
    controller = Controller(config, model)
    controller.update_draft("Please inspect authentication.")
    await asyncio.wait_for(model.explorer_started.wait(), timeout=1)
    assert controller._explorer_task is not None
    await controller._explorer_task

    result = await controller.commit()
    assert result == "Implemented using prior evidence"
    assert len(controller.buffer.indexes()) == 1
    await controller.close()


@pytest.mark.asyncio
async def test_stop_cancels_active_committed_run(tmp_path):
    class SlowModel(ScriptedModel):
        async def generate(self, *, system, messages, tools=()):
            if "committed coding agent" in system:
                await asyncio.sleep(10)
            return await super().generate(system=system, messages=messages, tools=tools)

    controller = Controller(
        HGIConfig(workspace=tmp_path, state_dir=tmp_path / ".state"), SlowModel()
    )
    task = asyncio.create_task(controller.commit("do work"))
    await asyncio.sleep(0.02)
    await controller.stop()
    with pytest.raises(asyncio.CancelledError):
        await task
    await controller.close()


@pytest.mark.asyncio
async def test_commit_reuses_partial_explorer_evidence(tmp_path):
    (tmp_path / "auth.py").write_text("def login():\n    return True\n")

    class PartialModel(ScriptedModel):
        def __init__(self):
            super().__init__()
            self.waiting_for_explorer_summary = asyncio.Event()

        async def generate(self, *, system, messages, tools=()):
            if "read-only coding repository explorer" in system:
                tool_results = [
                    block
                    for message in messages
                    for block in (
                        message["content"] if isinstance(message["content"], list) else []
                    )
                    if block["type"] == "tool_result"
                ]
                if not tool_results:
                    self.explorer_started.set()
                    return ModelReply(
                        tool_calls=[
                            ToolCall(
                                id="read-1",
                                name="read_file",
                                arguments={"path": "auth.py"},
                            )
                        ]
                    )
                self.waiting_for_explorer_summary.set()
                await asyncio.sleep(10)
            if "committed coding agent" in system:
                assert "def login" in messages[0]["content"]
                return ModelReply(text="Used partial evidence")
            return await super().generate(system=system, messages=messages, tools=tools)

    model = PartialModel()
    controller = Controller(
        HGIConfig(
            workspace=tmp_path,
            state_dir=tmp_path / ".state",
            pause_ms=100,
            min_judge_interval_ms=0,
        ),
        model,
    )
    controller.update_draft("Inspect authentication.")
    await asyncio.wait_for(model.waiting_for_explorer_summary.wait(), timeout=1)

    result = await controller.commit()

    assert result == "Used partial evidence"
    indexes = controller.buffer.indexes()
    assert len(indexes) == 1
    cached = controller.buffer.get(indexes[0].id)
    assert cached.provisional
    assert cached.confidence == 0.5
    assert cached.findings[0].path == "auth.py"
    await controller.close()


@pytest.mark.asyncio
async def test_explorer_starts_without_waiting_for_summary(tmp_path):
    class SlowSummaryModel(ScriptedModel):
        async def generate(self, *, system, messages, tools=()):
            if "Maintain a concise summary" in system:
                await asyncio.sleep(10)
            return await super().generate(system=system, messages=messages, tools=tools)

    model = SlowSummaryModel()
    controller = Controller(
        HGIConfig(
            workspace=tmp_path,
            state_dir=tmp_path / ".state",
            pause_ms=100,
            min_judge_interval_ms=0,
        ),
        model,
    )
    controller.update_draft("Inspect authentication.")

    await asyncio.wait_for(model.explorer_started.wait(), timeout=1)
    await controller.close()


@pytest.mark.asyncio
async def test_rephrased_goal_reuses_active_explorer_and_partial_evidence(tmp_path):
    (tmp_path / "tools.py").write_text("def write_file():\n    pass\n")

    class RephrasedGoalModel(ScriptedModel):
        def __init__(self):
            super().__init__()
            self.partial_ready = asyncio.Event()
            self.release = asyncio.Event()

        async def generate(self, *, system, messages, tools=()):
            if "read-only coding repository explorer" in system:
                tool_results = [
                    block
                    for message in messages
                    for block in (
                        message["content"] if isinstance(message["content"], list) else []
                    )
                    if block["type"] == "tool_result"
                ]
                if not tool_results:
                    return ModelReply(
                        tool_calls=[
                            ToolCall(
                                id="read-tools",
                                name="read_file",
                                arguments={"path": "tools.py"},
                            )
                        ]
                    )
                self.partial_ready.set()
                await self.release.wait()
                return ModelReply(
                    text='{"summary":"permission path found","findings":[], '
                    '"unresolved":[],"confidence":0.8}'
                )
            return await super().generate(system=system, messages=messages, tools=tools)

    model = RephrasedGoalModel()
    controller = Controller(
        HGIConfig(workspace=tmp_path, state_dir=tmp_path / ".state"), model
    )
    first_goal = (
        "审查 HGI 从 CLI 的 writes 参数到 write_file 和 run_command 的完整权限链路，"
        "说明 allow、ask、deny，并检查路径逃逸和 shell 注入防护。"
    )
    rephrased_goal = (
        "只读分析 HGI 的 CLI writes 配置如何传递到 write_file、run_command，"
        "明确 allow、ask、deny，并核查 shell 注入及路径逃逸防护。"
    )

    assert controller._start_explorer(first_goal)
    await asyncio.wait_for(model.partial_ready.wait(), timeout=1)
    original_task = controller._explorer_task
    partial_id = controller._partial_artifact.id

    assert not controller._start_explorer(rephrased_goal)
    assert controller._explorer_task is original_task
    assert controller._partial_artifact.id == partial_id
    assert not original_task.done()

    model.release.set()
    await asyncio.wait_for(original_task, timeout=1)
    await controller.close()


@pytest.mark.asyncio
async def test_explorer_finalizes_when_turn_budget_is_exhausted(tmp_path):
    class BudgetModel:
        def __init__(self):
            self.explorer_tool_visibility = []

        async def generate(self, *, system, messages, tools=()):
            if "read-only coding repository explorer" not in system:
                raise AssertionError(system)
            self.explorer_tool_visibility.append(bool(tools))
            if tools:
                call_number = len(self.explorer_tool_visibility)
                return ModelReply(
                    tool_calls=[
                        ToolCall(
                            id=f"glob-{call_number}",
                            name="glob_files",
                            arguments={"pattern": "**/*.py"},
                        )
                    ]
                )
            return ModelReply(
                text='{"summary":"budgeted result","findings":[], '
                '"unresolved":[],"confidence":0.6}'
            )

    model = BudgetModel()
    controller = Controller(
        HGIConfig(
            workspace=tmp_path,
            state_dir=tmp_path / ".state",
            explorer_soft_turns=2,
            max_explorer_turns=3,
        ),
        model,
    )

    assert controller._start_explorer("inventory Python files")
    await asyncio.wait_for(controller._explorer_task, timeout=1)

    assert model.explorer_tool_visibility == [True, False]
    assert controller.buffer.indexes() == []
    assert controller._partial_artifact.summary == "budgeted result"
    await controller.close()


@pytest.mark.asyncio
async def test_next_session_injects_version_validated_cached_evidence(tmp_path):
    (tmp_path / "auth.py").write_text("TOKEN = True\n")
    state_dir = tmp_path / ".state"
    buffer = SpeculativeBuffer(state_dir, workspace=tmp_path)
    assert buffer.put(
        Artifact(
            id="Acached",
            generation=1,
            goal="inspect authentication token",
            summary="authentication token is defined",
            findings=[
                Evidence(
                    path="auth.py",
                    line=1,
                    claim="cached token definition",
                    excerpt="1: TOKEN = True",
                )
            ],
            confidence=0.9,
        )
    )

    class CacheAwareModel:
        async def generate(self, *, system, messages, tools=()):
            assert "read-only coding repository explorer" in system
            prompt = messages[0]["content"]
            assert "Verified prior artifacts" in prompt
            assert "cached token definition" in prompt
            assert "content_hash" not in prompt
            return ModelReply(
                text='{"summary":"cache reused","findings":[], '
                '"unresolved":[],"confidence":0.8}'
            )

    controller = Controller(
        HGIConfig(workspace=tmp_path, state_dir=state_dir), CacheAwareModel()
    )

    assert controller._start_explorer("inspect authentication token")
    await asyncio.wait_for(controller._explorer_task, timeout=1)

    assert controller.logger.metrics_for(controller._run_id)["artifact_cache.hits"] == 1
    await controller.close()


@pytest.mark.asyncio
async def test_completed_run_log_contains_aggregated_usage_without_content(tmp_path):
    class UsageModel:
        model = "usage-model"

        async def generate(self, *, system, messages, tools=()):
            text = "summary" if "Maintain a concise summary" in system else "private answer"
            return ModelReply(
                text=text,
                usage=ModelUsage(
                    input_tokens=10,
                    output_tokens=4,
                    reasoning_tokens=2,
                    total_tokens=14,
                ),
            )

    state_dir = tmp_path / ".state"
    controller = Controller(HGIConfig(workspace=tmp_path, state_dir=state_dir), UsageModel())
    result = await controller.commit("private user request")
    await controller.close()

    log_text = (state_dir / "logs" / "hgi.jsonl").read_text()
    events = [json.loads(line) for line in log_text.splitlines()]
    completed = next(event for event in events if event["event"] == "run.completed")
    assert result == "private answer"
    assert completed["usage"]["total_tokens"] == 14
    assert "summary" not in completed["usage_by_component"]
    assert completed["usage_by_component"]["committed"]["total_tokens"] == 14
    assert "private user request" not in log_text
    assert "private answer" not in log_text


@pytest.mark.asyncio
async def test_commit_does_not_wait_for_redundant_summary(tmp_path):
    class SlowSummaryModel:
        async def generate(self, *, system, messages, tools=()):
            if "Maintain a concise summary" in system:
                await asyncio.sleep(10)
            if "committed coding agent" in system:
                return ModelReply(text="committed immediately")
            raise AssertionError(system)

    controller = Controller(
        HGIConfig(workspace=tmp_path, state_dir=tmp_path / ".state"),
        SlowSummaryModel(),
    )

    result = await asyncio.wait_for(controller.commit("complete request"), timeout=1)

    assert result == "committed immediately"
    await controller.close()


@pytest.mark.asyncio
async def test_commit_uses_summary_instead_of_fully_summarized_request(tmp_path):
    original = "irrelevant background " * 300 + "inspect Controller artifact selection"
    captured_prompt = ""

    class CapturingModel:
        async def generate(self, *, system, messages, tools=()):
            nonlocal captured_prompt
            assert "committed coding agent" in system
            captured_prompt = messages[0]["content"]
            return ModelReply(text="done")

    controller = Controller(
        HGIConfig(workspace=tmp_path, state_dir=tmp_path / ".state"),
        CapturingModel(),
    )
    controller.draft = original
    controller._summarized_draft = original
    controller.user_summary = "Inspect Controller artifact selection without modifying files."

    assert await controller.commit(original) == "done"

    assert controller.user_summary == ""
    assert "Inspect Controller artifact selection" in captured_prompt
    assert "irrelevant background" not in captured_prompt
    await controller.close()


@pytest.mark.asyncio
async def test_commit_appends_only_unsummarized_input_to_summary(tmp_path):
    summarized = "old background " * 300
    latest = "Inspect CacheEntry TTL and do not modify files."
    request = summarized + latest
    captured_prompt = ""

    class CapturingModel:
        async def generate(self, *, system, messages, tools=()):
            nonlocal captured_prompt
            assert "committed coding agent" in system
            captured_prompt = messages[0]["content"]
            return ModelReply(text="done")

    controller = Controller(
        HGIConfig(workspace=tmp_path, state_dir=tmp_path / ".state"),
        CapturingModel(),
    )
    controller.draft = request
    controller._summarized_draft = summarized
    controller.user_summary = "Ignore planning history and perform a read-only code analysis."

    assert await controller.commit(request) == "done"

    assert "Ignore planning history" in captured_prompt
    assert latest in captured_prompt
    assert "old background" not in captured_prompt
    await controller.close()


@pytest.mark.asyncio
async def test_read_only_commit_persists_source_evidence_for_followup(tmp_path):
    (tmp_path / "auth.py").write_text("TOKEN = True\n")

    class ReadingModel:
        async def generate(self, *, system, messages, tools=()):
            assert "committed coding agent" in system
            tool_results = [
                block
                for message in messages
                for block in (
                    message["content"] if isinstance(message["content"], list) else []
                )
                if block["type"] == "tool_result"
            ]
            if not tool_results:
                return ModelReply(
                    tool_calls=[
                        ToolCall(
                            id="read-auth",
                            name="read_file",
                            arguments={"path": "auth.py"},
                        )
                    ]
                )
            return ModelReply(text="Token is enabled")

    controller = Controller(
        HGIConfig(workspace=tmp_path, state_dir=tmp_path / ".state"),
        ReadingModel(),
    )

    assert await controller.commit("inspect authentication token") == "Token is enabled"

    indexes = controller.buffer.indexes()
    assert len(indexes) == 1
    cached = controller.buffer.get(indexes[0].id)
    assert cached.provisional
    assert cached.goal == "inspect authentication token"
    assert cached.findings[0].path == "auth.py"
    assert cached.findings[0].content_hash

    assert await controller.commit("inspect authentication token again") == "Token is enabled"
    assert len(controller.buffer.indexes()) == 1
    await controller.close()


@pytest.mark.asyncio
async def test_successful_edit_refreshes_and_replaces_stale_artifact(tmp_path):
    source = tmp_path / "auth.py"
    source.write_text("TOKEN = 'old'\n")
    state_dir = tmp_path / ".state"
    buffer = SpeculativeBuffer(state_dir, workspace=tmp_path)
    assert buffer.put(
        Artifact(
            id="Aold",
            generation=1,
            goal="update authentication token",
            summary="old token evidence",
            findings=[
                Evidence(
                    path="auth.py",
                    line=1,
                    claim="old token",
                    excerpt="1: TOKEN = 'old'",
                )
            ],
            confidence=0.9,
        )
    )
    old_hash = buffer.get("Aold").findings[0].content_hash

    class EditingModel:
        async def generate(self, *, system, messages, tools=()):
            tool_results = [
                block
                for message in messages
                for block in (
                    message["content"] if isinstance(message["content"], list) else []
                )
                if block["type"] == "tool_result"
            ]
            if "committed coding agent" in system:
                if not tool_results:
                    return ModelReply(
                        tool_calls=[
                            ToolCall(
                                id="write-auth",
                                name="write_file",
                                arguments={
                                    "path": "auth.py",
                                    "content": "TOKEN = 'new'\n",
                                },
                            )
                        ]
                    )
                return ModelReply(text="Updated authentication token")
            if "read-only coding repository explorer" in system:
                if not tool_results:
                    assert "Stale evidence must be refreshed: auth.py" in messages[0][
                        "content"
                    ]
                    return ModelReply(
                        tool_calls=[
                            ToolCall(
                                id="read-auth",
                                name="read_file",
                                arguments={"path": "auth.py"},
                            )
                        ]
                    )
                return ModelReply(
                    text=(
                        '{"summary":"new token evidence","findings":['
                        '{"path":"auth.py","line":1,"claim":"new token"}],'
                        '"unresolved":[],"confidence":0.9}'
                    )
                )
            raise AssertionError(system)

    controller = Controller(
        HGIConfig(
            workspace=tmp_path,
            state_dir=state_dir,
            writes=WritePolicy.ALLOW,
        ),
        EditingModel(),
    )

    result = await controller.commit("update authentication token")
    assert result == "Updated authentication token"
    assert controller._refresh_task is not None
    await asyncio.wait_for(controller._refresh_task, timeout=1)

    indexes = controller.buffer.indexes()
    assert len(indexes) == 1
    assert indexes[0].id != "Aold"
    refreshed = controller.buffer.get(indexes[0].id)
    assert refreshed.goal == "update authentication token"
    assert refreshed.findings[0].content_hash != old_hash
    assert refreshed.findings[0].claim == "new token"
    await controller.close()


@pytest.mark.asyncio
async def test_new_input_cancels_background_artifact_refresh(tmp_path):
    class SlowRefreshModel:
        def __init__(self):
            self.refresh_started = asyncio.Event()

        async def generate(self, *, system, messages, tools=()):
            tool_results = [
                block
                for message in messages
                for block in (
                    message["content"] if isinstance(message["content"], list) else []
                )
                if block["type"] == "tool_result"
            ]
            if "committed coding agent" in system:
                if not tool_results:
                    return ModelReply(
                        tool_calls=[
                            ToolCall(
                                id="write-file",
                                name="write_file",
                                arguments={"path": "app.py", "content": "VALUE = 2\n"},
                            )
                        ]
                    )
                return ModelReply(text="Updated app")
            if "read-only coding repository explorer" in system:
                self.refresh_started.set()
                await asyncio.sleep(10)
            raise AssertionError(system)

    model = SlowRefreshModel()
    controller = Controller(
        HGIConfig(
            workspace=tmp_path,
            state_dir=tmp_path / ".state",
            writes=WritePolicy.ALLOW,
        ),
        model,
    )
    await controller.commit("update app value")
    refresh_task = controller._refresh_task
    await asyncio.wait_for(model.refresh_started.wait(), timeout=1)

    controller.update_draft("a different follow-up")

    with pytest.raises(asyncio.CancelledError):
        await refresh_task
    await controller.close()


@pytest.mark.asyncio
async def test_failed_edit_does_not_schedule_background_refresh(tmp_path):
    class FailingEditModel:
        async def generate(self, *, system, messages, tools=()):
            tool_results = [
                block
                for message in messages
                for block in (
                    message["content"] if isinstance(message["content"], list) else []
                )
                if block["type"] == "tool_result"
            ]
            if "committed coding agent" not in system:
                raise AssertionError(system)
            if not tool_results:
                return ModelReply(
                    tool_calls=[
                        ToolCall(
                            id="write-file",
                            name="write_file",
                            arguments={"path": "app.py", "content": "VALUE = 2\n"},
                        )
                    ]
                )
            raise RuntimeError("model failed after write")

    controller = Controller(
        HGIConfig(
            workspace=tmp_path,
            state_dir=tmp_path / ".state",
            writes=WritePolicy.ALLOW,
        ),
        FailingEditModel(),
    )

    with pytest.raises(RuntimeError, match="model failed after write"):
        await controller.commit("update app value")

    assert controller._refresh_task is None
    await controller.close()


@pytest.mark.asyncio
async def test_artifact_refresh_retries_and_persists_recovery(tmp_path):
    (tmp_path / "auth.py").write_text("TOKEN = 'current'\n")

    class UnusedModel:
        async def generate(self, *, system, messages, tools=()):
            raise AssertionError("model should be replaced by the explorer stub")

    controller = Controller(
        HGIConfig(
            workspace=tmp_path,
            state_dir=tmp_path / ".state",
            artifact_refresh_retries=1,
            artifact_refresh_retry_delay_seconds=0,
        ),
        UnusedModel(),
    )
    attempts = 0

    async def explore(goal, generation, related):
        nonlocal attempts
        attempts += 1
        if attempts == 1:
            raise RuntimeError("temporary provider failure")
        return Artifact(
            id="Arecovered",
            generation=generation,
            goal=goal,
            summary="current authentication evidence",
            findings=[
                Evidence(
                    path="auth.py",
                    line=1,
                    claim="current token",
                    excerpt="1: TOKEN = 'current'",
                )
            ],
            confidence=0.9,
        )

    controller.explorer.explore = explore

    await controller._run_artifact_refresh("inspect authentication", {"auth.py"})

    assert attempts == 2
    cached = controller.buffer.get("Arecovered")
    assert cached.goal == "inspect authentication"
    assert not cached.provisional
    await controller.close()


@pytest.mark.asyncio
async def test_artifact_refresh_timeout_keeps_cache_unmodified(tmp_path):
    class UnusedModel:
        async def generate(self, *, system, messages, tools=()):
            raise AssertionError("model should be replaced by the explorer stub")

    controller = Controller(
        HGIConfig(
            workspace=tmp_path,
            state_dir=tmp_path / ".state",
            artifact_refresh_timeout_seconds=1,
            artifact_refresh_retries=0,
        ),
        UnusedModel(),
    )

    async def explore(_goal, _generation, _related):
        await asyncio.sleep(10)

    controller.explorer.explore = explore
    started = asyncio.get_running_loop().time()

    await controller._run_artifact_refresh("inspect authentication", {"auth.py"})

    assert asyncio.get_running_loop().time() - started < 2
    assert controller.buffer.indexes() == []
    await controller.close()

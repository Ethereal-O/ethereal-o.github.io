from __future__ import annotations

import asyncio
import inspect
import re
import time
from collections.abc import Awaitable, Callable
from pathlib import Path
from uuid import uuid4

from .buffer import SpeculativeBuffer
from .config import ControlMode, HGIConfig
from .model import ModelBackend
from .observability import InstrumentedModelBackend, StructuredLogger, log_context
from .schemas import Artifact, Evidence, JudgeAction, RunStatus, RuntimeEvent
from .services import (
    CommittedService,
    ExplorerService,
    JudgeService,
    SummaryService,
    artifact_reader,
)
from .tools import PermissionCallback, WorkspaceTools

EventCallback = Callable[[RuntimeEvent], None | Awaitable[None]]
BOUNDARIES = frozenset(".!?。！？;；\n")


def _goal_terms(value: str) -> set[str]:
    lowered = value.lower()
    terms = {term for term in re.findall(r"[a-z0-9_./-]+", lowered) if len(term) > 1}
    for run in re.findall(r"[\u3400-\u9fff]+", lowered):
        terms.update(run[index : index + 2] for index in range(len(run) - 1))
    return terms


def _goal_similarity(left: str, right: str) -> float:
    left_terms = _goal_terms(left)
    right_terms = _goal_terms(right)
    if not left_terms or not right_terms:
        return 1.0 if left.strip().lower() == right.strip().lower() else 0.0

    def is_cjk(term: str) -> bool:
        return any("\u3400" <= character <= "\u9fff" for character in term)

    left_identifiers = {term for term in left_terms if not is_cjk(term)}
    right_identifiers = {term for term in right_terms if not is_cjk(term)}
    shared_identifiers = left_identifiers & right_identifiers
    overall = len(left_terms & right_terms) / min(len(left_terms), len(right_terms))
    if len(shared_identifiers) < 3:
        return overall

    identifier_score = len(shared_identifiers) / min(
        len(left_identifiers), len(right_identifiers)
    )
    left_cjk = left_terms - left_identifiers
    right_cjk = right_terms - right_identifiers
    cjk_score = (
        len(left_cjk & right_cjk) / min(len(left_cjk), len(right_cjk))
        if left_cjk and right_cjk
        else 0.0
    )
    return max(overall, identifier_score * 0.7 + cjk_score * 0.3)


class Controller:
    def __init__(
        self,
        config: HGIConfig,
        model: ModelBackend,
        *,
        on_event: EventCallback | None = None,
        permission: PermissionCallback | None = None,
    ):
        self.config = config
        self.on_event = on_event
        state_dir = config.state_dir or Path(".hgi")
        self.logger = StructuredLogger(
            state_dir / "logs" / "hgi.jsonl",
            level=config.log_level,
            max_bytes=config.log_max_bytes,
            backup_count=config.log_backup_count,
            enabled=config.log_enabled,
        )
        self.model = InstrumentedModelBackend(model, self.logger)
        self.buffer = SpeculativeBuffer(
            config.state_dir or Path(".hgi"), workspace=config.workspace
        )
        workspace_tools = WorkspaceTools(
            config.workspace,
            write_policy=config.writes,
            permission=permission,
            on_mutation=self._on_workspace_mutation,
            logger=self.logger,
        )
        explorer_dispatcher = workspace_tools.readonly([artifact_reader(self.buffer)])
        self.judge = JudgeService(self.model)
        self.summary_service = SummaryService(self.model)
        self.explorer = ExplorerService(
            self.model,
            explorer_dispatcher,
            self.buffer,
            soft_turns=min(config.explorer_soft_turns, config.max_agent_turns),
            max_turns=min(config.max_agent_turns, config.max_explorer_turns),
            max_tool_result_chars=config.max_explorer_tool_chars,
        )
        self.committed = CommittedService(
            self.model,
            workspace_tools.committed(),
            max_turns=config.max_agent_turns,
            max_tool_result_chars=config.max_committed_tool_chars,
        )
        self.draft = ""
        self.user_summary = ""
        self._summarized_draft = ""
        self._version = 0
        self._judged_version = -1
        self._last_judge_at = 0.0
        self._generation = 0
        self._pause_task: asyncio.Task[None] | None = None
        self._judge_task: asyncio.Task[None] | None = None
        self._pending_judge_version: int | None = None
        self._explorer_task: asyncio.Task[None] | None = None
        self._explorer_goal: str | None = None
        self._partial_artifact: Artifact | None = None
        self._committed_task: asyncio.Task[str] | None = None
        self._refresh_task: asyncio.Task[None] | None = None
        self._changed_paths: set[str] = set()
        self._workspace_dirty = False
        self._run_id: str | None = None
        self._closed = False
        self.logger.log(
            "session.started",
            workspace=str(config.workspace),
            auto_explore=config.auto_explore,
            commit_mode=config.commit_mode,
            interrupt_mode=config.interrupt_mode,
            write_policy=config.writes,
        )

    @property
    def runtime_state(self) -> str:
        explorer = "running" if self._explorer_task and not self._explorer_task.done() else "idle"
        committed = "running" if self._committed_task and not self._committed_task.done() else "idle"
        return f"explorer={explorer}; committed={committed}; generation={self._generation}"

    def update_draft(self, text: str) -> None:
        if self._closed or text == self.draft:
            return
        if self._refresh_task and not self._refresh_task.done():
            self._refresh_task.cancel()
        self.draft = text
        if self._run_id is None and text.strip():
            self._run_id = uuid4().hex
            self.logger.log("input.started", run_id=self._run_id, input_chars=len(text))
        self._version += 1
        version = self._version
        if self._pause_task:
            self._pause_task.cancel()
        self._pause_task = asyncio.create_task(self._after_pause(version))
        if text and text[-1] in BOUNDARIES:
            self._schedule_judge(version)
        if len(text) - len(self._summarized_draft) >= self.config.summary_checkpoint_chars:
            asyncio.create_task(self._update_summary())

    async def _after_pause(self, version: int) -> None:
        try:
            await asyncio.sleep(self.config.pause_ms / 1000)
            self._schedule_judge(version)
        except asyncio.CancelledError:
            return

    def _schedule_judge(self, version: int) -> None:
        if version != self._version or version == self._judged_version or not self.draft.strip():
            return
        if self._judge_task and not self._judge_task.done():
            self._pending_judge_version = version
            return
        self._judge_task = asyncio.create_task(self._run_judge(version))
        self._judge_task.add_done_callback(self._judge_finished)

    def _judge_finished(self, task: asyncio.Task[None]) -> None:
        if self._judge_task is task:
            self._judge_task = None
        pending = self._pending_judge_version
        self._pending_judge_version = None
        if pending is not None and not self._closed:
            self._schedule_judge(pending)

    async def _run_judge(self, version: int) -> None:
        with log_context(run_id=self._ensure_run_id(), component="judge", input_version=version):
            await self._run_judge_in_context(version)

    async def _run_judge_in_context(self, version: int) -> None:
        elapsed = time.monotonic() - self._last_judge_at
        delay = self.config.min_judge_interval_ms / 1000 - elapsed
        if delay > 0:
            await asyncio.sleep(delay)
        if version != self._version or self._judged_version == version:
            return
        self._judged_version = version
        self._last_judge_at = time.monotonic()
        recent = self._recent_input()
        try:
            decision = await self.judge.decide(self.user_summary, recent, self.runtime_state)
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            await self._emit(RuntimeEvent(kind="error", message=f"Judge failed: {exc}"))
            return
        await self._emit(
            RuntimeEvent(kind="judge", message=decision.reason, data=decision.model_dump(mode="json"))
        )
        if version != self._version:
            return
        if decision.action == JudgeAction.SUMMARIZE_AND_WAIT:
            await self._update_summary()
        elif decision.action == JudgeAction.EXPLORE and self.config.auto_explore:
            goal = decision.exploration_goal or recent
            self._start_explorer(goal)
            await self._update_summary()
        elif decision.action == JudgeAction.COMMIT:
            if self.config.commit_mode == ControlMode.AUTO:
                await self.commit(self.draft)
            elif self.config.commit_mode == ControlMode.ASSIST or self.config.judge_shadow_control:
                await self._emit(
                    RuntimeEvent(kind="recommend_commit", message="The request appears complete")
                )

    def _recent_input(self) -> str:
        if self.draft.startswith(self._summarized_draft):
            return self.draft[len(self._summarized_draft) :]
        return self.draft

    async def _update_summary(self) -> None:
        snapshot = self.draft
        if not snapshot or snapshot == self._summarized_draft:
            return
        extends = snapshot.startswith(self._summarized_draft)
        old = self.user_summary if extends else ""
        new_input = snapshot[len(self._summarized_draft) :] if extends else snapshot
        try:
            with log_context(run_id=self._ensure_run_id(), component="summary"):
                updated = await self.summary_service.update(old, new_input)
        except Exception as exc:
            await self._emit(RuntimeEvent(kind="error", message=f"Summary failed: {exc}"))
            return
        if self.draft.startswith(snapshot):
            self.user_summary = updated
            self._summarized_draft = snapshot
            await self._emit(RuntimeEvent(kind="summary", message=updated))

    def _start_explorer(self, goal: str) -> bool:
        if not goal.strip():
            return False
        similarity = (
            _goal_similarity(self._explorer_goal, goal) if self._explorer_goal else 0.0
        )
        active = self._explorer_task is not None and not self._explorer_task.done()
        has_evidence = self._partial_artifact is not None and bool(self._partial_artifact.findings)
        if self._explorer_goal and similarity >= self.config.exploration_goal_similarity:
            if active or has_evidence:
                self.logger.log(
                    "explorer.reused",
                    similarity=round(similarity, 3),
                    active=active,
                    has_evidence=has_evidence,
                )
                self.logger.increment("explorer.reused")
                return False
        if active:
            if not self.config.auto_cancel_explorer:
                return False
            self._explorer_task.cancel()
        if self._partial_artifact and self._partial_artifact.findings:
            self.buffer.put(self._partial_artifact)
        self._generation += 1
        generation = self._generation
        self._partial_artifact = None
        self._explorer_goal = goal
        related_artifacts = self.buffer.search(
            goal, self.config.artifact_limit, include_stale=True
        )
        related = [item.id for item in related_artifacts]
        superseded = [
            item.id
            for item in related_artifacts
            if _goal_similarity(item.goal, goal) >= self.config.exploration_goal_similarity
        ]
        if related:
            with log_context(run_id=self._ensure_run_id(), component="controller"):
                self.logger.log("artifact.cache.hit", artifact_count=len(related))
                self.logger.increment("artifact_cache.hits", len(related))
        self._explorer_task = asyncio.create_task(
            self._run_explorer(goal, generation, related, superseded),
            name=f"hgi-explorer-{generation}",
        )
        return True

    async def _run_explorer(
        self,
        goal: str,
        generation: int,
        related: list[str],
        superseded: list[str],
    ) -> None:
        with log_context(
            run_id=self._ensure_run_id(), component="explorer", generation=generation
        ):
            await self._run_explorer_in_context(goal, generation, related, superseded)

    async def _run_explorer_in_context(
        self,
        goal: str,
        generation: int,
        related: list[str],
        superseded: list[str],
    ) -> None:
        await self._emit(
            RuntimeEvent(kind="explorer", status=RunStatus.RUNNING, message=goal)
        )
        try:
            def record_progress(artifact):
                if generation == self._generation:
                    self._partial_artifact = artifact

            artifact = await self.explorer.explore(
                goal, generation, related, on_progress=record_progress
            )
            if generation != self._generation:
                return
            self._partial_artifact = artifact
            persisted = self.buffer.put(artifact, supersedes=superseded)
            if persisted:
                self._partial_artifact = self.buffer.get(artifact.id)
            self.logger.increment(
                "artifact_cache.persisted" if persisted else "artifact_cache.rejected"
            )
            await self._emit(
                RuntimeEvent(
                    kind="explorer",
                    status=RunStatus.COMPLETE,
                    message=artifact.summary,
                    data={"artifact_id": artifact.id, "persisted": persisted},
                )
            )
        except asyncio.CancelledError:
            await self._emit(
                RuntimeEvent(kind="explorer", status=RunStatus.CANCELLED, message=goal)
            )
            raise
        except Exception as exc:
            await self._emit(
                RuntimeEvent(kind="explorer", status=RunStatus.FAILED, message=str(exc))
            )

    async def commit(self, request: str | None = None) -> str:
        request = request if request is not None else self.draft
        if not request.strip():
            return ""
        run_id = self._ensure_run_id()
        with log_context(run_id=run_id, component="commit"):
            return await self._commit_in_context(request, run_id)

    def _request_for_commit(self, request: str) -> str:
        """Use summarized intent without dropping input that has not been summarized yet."""
        if not self.user_summary or not self._summarized_draft:
            return request
        if request == self._summarized_draft:
            return self.user_summary
        if request.startswith(self._summarized_draft):
            recent = request[len(self._summarized_draft) :].strip()
            if recent:
                return (
                    f"User intent summary:\n{self.user_summary}\n\n"
                    f"Unsummarized latest input:\n{recent}"
                )
            return self.user_summary
        return request

    async def _commit_in_context(self, request: str, run_id: str) -> str:
        started = time.perf_counter()
        refresh_request: tuple[str, set[str] | None] | None = None
        if self._refresh_task and not self._refresh_task.done():
            self._refresh_task.cancel()
            await asyncio.gather(self._refresh_task, return_exceptions=True)
        self._changed_paths.clear()
        self._workspace_dirty = False
        if self._pause_task:
            self._pause_task.cancel()
        self._pending_judge_version = None
        self._judged_version = self._version
        current = asyncio.current_task()
        if self._judge_task and self._judge_task is not current and not self._judge_task.done():
            self._judge_task.cancel()
        committed_request = self._request_for_commit(request)
        self.logger.log(
            "run.started",
            request_chars=len(request),
            committed_request_chars=len(committed_request),
            summary_applied=committed_request != request,
        )
        partial_artifact = self._partial_artifact
        if self._explorer_task and not self._explorer_task.done():
            self._generation += 1
            self._explorer_task.cancel()
        self.draft = request
        artifacts = self.buffer.search(committed_request, self.config.artifact_limit)
        if partial_artifact and partial_artifact.findings:
            artifacts = [partial_artifact] + [
                artifact for artifact in artifacts if artifact.id != partial_artifact.id
            ]
            artifacts = artifacts[: self.config.artifact_limit]
        await self._emit(
            RuntimeEvent(
                kind="committed",
                status=RunStatus.RUNNING,
                message=request,
                data={"artifact_ids": [item.id for item in artifacts]},
            )
        )
        committed_evidence: list[Evidence] = []

        def capture_committed_evidence(call, output):
            existing = {
                (item.path, item.line, item.claim) for item in committed_evidence
            }
            committed_evidence.extend(
                item
                for item in ExplorerService._evidence_from_tool(call, output)
                if (item.path, item.line, item.claim) not in existing
            )

        self._committed_task = asyncio.create_task(
            self.committed.run(
                committed_request,
                artifacts,
                on_tool_result=capture_committed_evidence,
            )
        )
        try:
            result = await self._committed_task
            existing_ids = {index.id for index in self.buffer.indexes()}
            partial_is_new = partial_artifact and partial_artifact.id not in existing_ids
            if not self._workspace_dirty and not self._changed_paths and (
                partial_is_new or (committed_evidence and not artifacts)
            ):
                provisional = (
                    partial_artifact.model_copy(deep=True)
                    if partial_is_new
                    else Artifact(
                        id=f"A{uuid4().hex[:10]}",
                        generation=self._generation,
                        goal=request,
                        summary="Provisional source evidence from a successful read-only run.",
                    )
                )
                provisional.goal = committed_request
                provisional.findings = ExplorerService._merge_findings(
                    committed_evidence, provisional.findings
                )
                provisional.confidence = max(provisional.confidence, 0.5)
                provisional.provisional = True
                marker = "Provisional source evidence from a successful read-only run."
                if marker not in provisional.unresolved:
                    provisional.unresolved.append(marker)
                persisted = self.buffer.put(provisional)
                self.logger.increment(
                    "artifact_cache.provisional_persisted"
                    if persisted
                    else "artifact_cache.provisional_rejected"
                )
            await self._emit(
                RuntimeEvent(kind="committed", status=RunStatus.COMPLETE, message=result)
            )
            self.logger.log(
                "run.completed",
                duration_ms=round((time.perf_counter() - started) * 1000, 3),
                answer_chars=len(result),
                artifact_count=len(artifacts),
                usage=self.logger.usage_for(run_id),
                usage_by_component=self.logger.usage_breakdown_for(run_id),
                metrics=self.logger.metrics_for(run_id),
            )
            if self._workspace_dirty or self._changed_paths:
                refresh_request = (
                    committed_request,
                    None if self._workspace_dirty else set(self._changed_paths),
                )
            return result
        except asyncio.CancelledError:
            await self._emit(
                RuntimeEvent(kind="committed", status=RunStatus.CANCELLED, message=request)
            )
            self.logger.log(
                "run.cancelled",
                level="WARNING",
                duration_ms=round((time.perf_counter() - started) * 1000, 3),
                usage=self.logger.usage_for(run_id),
                usage_by_component=self.logger.usage_breakdown_for(run_id),
                metrics=self.logger.metrics_for(run_id),
            )
            raise
        except Exception as exc:
            await self._emit(
                RuntimeEvent(kind="committed", status=RunStatus.FAILED, message=str(exc))
            )
            self.logger.log(
                "run.failed",
                level="ERROR",
                duration_ms=round((time.perf_counter() - started) * 1000, 3),
                error_type=type(exc).__name__,
                error=str(exc),
                usage=self.logger.usage_for(run_id),
                usage_by_component=self.logger.usage_breakdown_for(run_id),
                metrics=self.logger.metrics_for(run_id),
            )
            raise
        finally:
            self.logger.usage_for(run_id, clear=True)
            self.logger.usage_breakdown_for(run_id, clear=True)
            self.logger.metrics_for(run_id, clear=True)
            self.draft = ""
            self._summarized_draft = ""
            self.user_summary = ""
            self._partial_artifact = None
            self._explorer_goal = None
            self._run_id = None
            self._version += 1
            self._changed_paths.clear()
            self._workspace_dirty = False
            if refresh_request and not self._closed:
                self._schedule_artifact_refresh(*refresh_request)

    async def _on_workspace_mutation(self, paths: set[str] | None) -> None:
        if paths is None:
            self._workspace_dirty = True
            self._changed_paths.clear()
        elif not self._workspace_dirty:
            self._changed_paths.update(paths)
        invalidated = self.buffer.invalidate(paths)
        self.logger.log(
            "artifact.cache.invalidated",
            changed_path_count=len(paths) if paths is not None else None,
            invalidate_all=paths is None,
            artifact_count=invalidated,
        )
        self.logger.increment("artifact_cache.invalidated", invalidated)

    def _schedule_artifact_refresh(self, request: str, paths: set[str] | None) -> None:
        if self._refresh_task and not self._refresh_task.done():
            self._refresh_task.cancel()
        self._refresh_task = asyncio.create_task(
            self._run_artifact_refresh(request, paths), name="hgi-artifact-refresh"
        )

    async def _run_artifact_refresh(self, request: str, paths: set[str] | None) -> None:
        run_id = uuid4().hex
        started = time.perf_counter()
        self._generation += 1
        generation = self._generation
        changed = "the entire workspace" if paths is None else ", ".join(sorted(paths)[:50])
        goal = (
            f"Refresh cached repository evidence after a successful modification. "
            f"Original request: {request}\nChanged paths: {changed}. "
            "Read the changed files, refresh stale claims, and return current evidence only."
        )
        related_artifacts = self.buffer.search(
            request, self.config.artifact_limit, include_stale=True
        )
        related = [artifact.id for artifact in related_artifacts]
        superseded = [
            artifact.id
            for artifact in related_artifacts
            if _goal_similarity(artifact.goal, request)
            >= self.config.exploration_goal_similarity
        ]
        with log_context(run_id=run_id, component="refresh", generation=generation):
            self.logger.log(
                "artifact.refresh.started",
                changed_path_count=len(paths) if paths is not None else None,
                invalidate_all=paths is None,
                related_artifact_count=len(related),
            )
            try:
                artifact = None
                attempts = self.config.artifact_refresh_retries + 1
                for attempt in range(1, attempts + 1):
                    try:
                        artifact = await asyncio.wait_for(
                            self.explorer.explore(goal, generation, related),
                            timeout=self.config.artifact_refresh_timeout_seconds,
                        )
                        break
                    except asyncio.CancelledError:
                        raise
                    except Exception as exc:
                        self.logger.log(
                            "artifact.refresh.attempt_failed",
                            level="WARNING",
                            attempt=attempt,
                            attempts=attempts,
                            error_type=type(exc).__name__,
                        )
                        if attempt == attempts:
                            raise
                        await asyncio.sleep(
                            self.config.artifact_refresh_retry_delay_seconds
                        )
                assert artifact is not None
                # Keep the durable cache keyed to the user's topic, not this refresh job.
                artifact.goal = request
                artifact.provisional = False
                persisted = self.buffer.put(artifact, supersedes=superseded)
                self.logger.increment(
                    "artifact_cache.persisted" if persisted else "artifact_cache.rejected"
                )
                self.logger.log(
                    "artifact.refresh.completed",
                    duration_ms=round((time.perf_counter() - started) * 1000, 3),
                    persisted=persisted,
                    finding_count=len(artifact.findings),
                    usage=self.logger.usage_for(run_id),
                    metrics=self.logger.metrics_for(run_id),
                )
            except asyncio.CancelledError:
                self.logger.log(
                    "artifact.refresh.cancelled",
                    level="WARNING",
                    duration_ms=round((time.perf_counter() - started) * 1000, 3),
                    usage=self.logger.usage_for(run_id),
                    metrics=self.logger.metrics_for(run_id),
                )
                raise
            except Exception as exc:
                self.logger.log(
                    "artifact.refresh.failed",
                    level="ERROR",
                    duration_ms=round((time.perf_counter() - started) * 1000, 3),
                    error_type=type(exc).__name__,
                    error=str(exc),
                    usage=self.logger.usage_for(run_id),
                    metrics=self.logger.metrics_for(run_id),
                )
            finally:
                self.logger.usage_for(run_id, clear=True)
                self.logger.usage_breakdown_for(run_id, clear=True)
                self.logger.metrics_for(run_id, clear=True)

    async def stop(self) -> None:
        self._generation += 1
        self._pending_judge_version = None
        self._partial_artifact = None
        self._explorer_goal = None
        current = asyncio.current_task()
        tasks = [
            task
            for task in (
                self._pause_task,
                self._judge_task,
                self._explorer_task,
                self._committed_task,
                self._refresh_task,
            )
            if task and task is not current and not task.done()
        ]
        for task in tasks:
            task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)

    async def wait_for_commit(self) -> str:
        if self._committed_task is None:
            return ""
        return await asyncio.shield(self._committed_task)

    async def close(self) -> None:
        self._closed = True
        await self.stop()
        self.logger.log("session.closed")
        self.logger.close()

    async def _emit(self, event: RuntimeEvent) -> None:
        self.logger.log(
            "runtime.event",
            runtime_kind=event.kind,
            status=event.status,
            message_chars=len(event.message),
            data=event.data,
        )
        if self.on_event:
            result = self.on_event(event)
            if inspect.isawaitable(result):
                await result

    def _ensure_run_id(self) -> str:
        if self._run_id is None:
            self._run_id = uuid4().hex
        return self._run_id

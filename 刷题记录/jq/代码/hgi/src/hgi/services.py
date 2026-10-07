from __future__ import annotations

import json
import inspect
import re
from collections.abc import Awaitable, Callable
from typing import Any
from uuid import uuid4

from pydantic import ValidationError

from .agent import AgentLoop, ToolResultCallback
from .buffer import SpeculativeBuffer
from .model import ModelBackend
from .schemas import Artifact, Evidence, JudgeDecision, ToolCall
from .tools import Dispatcher, Tool


def _artifact_payload(artifact: Artifact) -> dict[str, Any]:
    payload = artifact.model_dump(mode="json")
    for finding in payload["findings"]:
        finding.pop("content_hash", None)
    return payload


def _artifact_context(artifacts: list[Artifact], max_chars: int = 16_000) -> str:
    context: list[dict[str, Any]] = []
    for artifact in artifacts:
        payload = _artifact_payload(artifact)
        findings = payload.pop("findings")
        payload["findings"] = []
        context.append(payload)
        if len(json.dumps(context, ensure_ascii=False)) > max_chars:
            context.pop()
            break
        for finding in findings:
            payload["findings"].append(finding)
            if len(json.dumps(context, ensure_ascii=False)) > max_chars:
                payload["findings"].pop()
                break
        if len(json.dumps(context, ensure_ascii=False)) > max_chars:
            context.pop()
            break
    return json.dumps(context, ensure_ascii=False)


def _json_object(text: str) -> dict[str, Any]:
    stripped = text.strip()
    fenced = re.search(r"```(?:json)?\s*(\{.*?\})\s*```", stripped, re.DOTALL)
    if fenced:
        stripped = fenced.group(1)
    else:
        start, end = stripped.find("{"), stripped.rfind("}")
        if start >= 0 and end > start:
            stripped = stripped[start : end + 1]
    value = json.loads(stripped)
    if not isinstance(value, dict):
        raise ValueError("expected a JSON object")
    return value


class JudgeService:
    SYSTEM = """You are HGI's input-boundary judge. Decide whether incomplete user typing
contains enough stable intent for useful read-only repository exploration. Return JSON only:
{"action":"wait|summarize_and_wait|explore|commit","exploration_goal":"...",
"reason":"...","confidence":0.0}. Prefer explore over commit. Commit means the request
appears complete, but policy may only show a recommendation. If an Explorer is already
running and new input only restates the same goal or adds irrelevant background, return wait;
only return explore again when the repository goal materially changes. Never infer write
permission."""

    def __init__(self, model: ModelBackend):
        self.model = model

    async def decide(self, summary: str, recent: str, runtime_state: str) -> JudgeDecision:
        reply = await self.model.generate(
            system=self.SYSTEM,
            messages=[
                {
                    "role": "user",
                    "content": (
                        f"User summary:\n{summary or '(empty)'}\n\n"
                        f"Recent unsummarized input:\n{recent or '(empty)'}\n\n"
                        f"Runtime state:\n{runtime_state}"
                    ),
                }
            ],
        )
        return JudgeDecision.model_validate(_json_object(reply.text))


class SummaryService:
    SYSTEM = """Maintain a concise summary of user intent. Consume only the previous user
summary and new user input. Do not invent repository facts or include agent/tool output.
Return the new summary as plain text."""

    def __init__(self, model: ModelBackend):
        self.model = model

    async def update(self, old_summary: str, new_input: str) -> str:
        if not new_input:
            return old_summary
        reply = await self.model.generate(
            system=self.SYSTEM,
            messages=[
                {
                    "role": "user",
                    "content": f"Previous user summary:\n{old_summary}\n\nNew user input:\n{new_input}",
                }
            ],
        )
        return reply.text.strip()


class ExplorerService:
    SYSTEM = """You are a read-only coding repository explorer. Use the available tools to
find concrete evidence relevant to the goal. You cannot edit files or run shell commands.
Treat prior artifacts as candidate evidence and verify anything important. Finish with JSON:
{"summary":"...","findings":[{"path":"...","line":1,"claim":"...","excerpt":"..."}],
"unresolved":["..."],"confidence":0.0}. Do not wrap the JSON in prose."""

    def __init__(
        self,
        model: ModelBackend,
        dispatcher: Dispatcher,
        buffer: SpeculativeBuffer,
        *,
        soft_turns: int,
        max_turns: int,
        max_tool_result_chars: int,
    ):
        self.loop = AgentLoop(
            model,
            dispatcher,
            max_turns=max_turns,
            soft_turns=soft_turns,
            finalize_on_limit=True,
            max_tool_result_chars=max_tool_result_chars,
        )
        self.buffer = buffer

    async def explore(
        self,
        goal: str,
        generation: int,
        related_ids: list[str] | None = None,
        on_progress: Callable[[Artifact], None | Awaitable[None]] | None = None,
    ) -> Artifact:
        artifact = Artifact(
            id=f"A{uuid4().hex[:10]}",
            generation=generation,
            goal=goal,
            summary="Partial read-only exploration in progress.",
            confidence=0.3,
        )
        high_value_findings = 0

        async def capture(call: ToolCall, output: str) -> None:
            new_evidence = self._evidence_from_tool(call, output)
            if not new_evidence:
                return
            existing = {(item.path, item.line, item.claim) for item in artifact.findings}
            artifact.findings.extend(
                item
                for item in new_evidence
                if (item.path, item.line, item.claim) not in existing
            )
            artifact.findings = self._limit_findings(artifact.findings)
            artifact.summary = (
                f"Partial exploration collected {len(artifact.findings)} repository evidence items."
            )
            if on_progress:
                result = on_progress(artifact.model_copy(deep=True))
                if inspect.isawaitable(result):
                    await result

        prompt = f"Exploration goal:\n{goal}"
        if related_ids:
            related = [self.buffer.get(artifact_id) for artifact_id in related_ids]
            prompt += (
                "\n\nVerified prior artifacts follow. Reuse unchanged evidence directly. "
                "Do not reread cited files unless an unresolved item requires it; focus on "
                "missing or stale evidence:\n"
                + _artifact_context(related)
            )

        def evidence_is_still_growing() -> bool:
            nonlocal high_value_findings
            current = sum(
                1
                for finding in artifact.findings
                if finding.excerpt and finding.path
            )
            growing = current > high_value_findings
            high_value_findings = current
            return growing

        output = await self.loop.run(
            system=self.SYSTEM,
            prompt=prompt,
            on_tool_result=capture,
            continue_after_soft_limit=evidence_is_still_growing,
        )
        try:
            data = _json_object(output)
            findings = [Evidence.model_validate(item) for item in data.get("findings", [])]
            summary = str(data.get("summary", ""))
            unresolved = [str(item) for item in data.get("unresolved", [])]
            confidence = float(data.get("confidence", 0.5))
        except (ValueError, TypeError, ValidationError):
            findings, unresolved, confidence = [], ["Explorer returned unstructured output"], 0.25
            summary = output.strip()[:4000]
        artifact.summary = summary
        artifact.findings = self._merge_findings(findings, artifact.findings)
        artifact.unresolved = unresolved
        artifact.confidence = confidence
        return artifact

    @staticmethod
    def _merge_findings(primary: list[Evidence], fallback: list[Evidence]) -> list[Evidence]:
        fallback_by_location = {(item.path, item.line): item for item in fallback}
        merged: list[Evidence] = []
        seen: set[tuple[str, int | None]] = set()
        for item in primary + fallback:
            key = (item.path, item.line)
            if key not in seen:
                if not item.excerpt and key in fallback_by_location:
                    item.excerpt = fallback_by_location[key].excerpt
                merged.append(item)
                seen.add(key)
        return ExplorerService._limit_findings(merged)

    @staticmethod
    def _limit_findings(findings: list[Evidence]) -> list[Evidence]:
        limited: list[Evidence] = []
        excerpt_chars = 0
        for item in findings:
            if len(limited) >= 40:
                break
            remaining = 24_000 - excerpt_chars
            if item.excerpt and remaining <= 0:
                item.excerpt = ""
            elif len(item.excerpt) > remaining:
                item.excerpt = item.excerpt[:remaining]
            excerpt_chars += len(item.excerpt)
            limited.append(item)
        return limited

    @staticmethod
    def _evidence_from_tool(call: ToolCall, output: str) -> list[Evidence]:
        if output.startswith("Error:"):
            return []
        if call.name == "read_file":
            return [
                Evidence(
                    path=str(call.arguments.get("path", "")),
                    line=int(call.arguments.get("start_line", 1)),
                    claim="Source content collected by the read-only Explorer.",
                    excerpt=output[:3500],
                )
            ]
        if call.name == "grep":
            findings: list[Evidence] = []
            for match in output.splitlines()[:20]:
                parts = match.split(":", 2)
                if len(parts) != 3 or not parts[1].isdigit():
                    continue
                findings.append(
                    Evidence(
                        path=parts[0],
                        line=int(parts[1]),
                        claim=f"Search match for {call.arguments.get('pattern', 'the query')}.",
                        excerpt=parts[2][:500],
                    )
                )
            return findings
        if call.name == "glob_files":
            return [
                Evidence(path=path, claim="File discovered in the workspace inventory.")
                for path in output.splitlines()[:30]
                if path
            ]
        if call.name == "git_diff" and output.strip():
            return [
                Evidence(
                    path=str(call.arguments.get("path", ".")),
                    claim="Uncommitted diff collected by the read-only Explorer.",
                    excerpt=output[:3500],
                )
            ]
        return []


class CommittedService:
    SYSTEM = """You are the committed coding agent. Complete the user's explicit request.
You have permission-controlled tools. Reuse supplied speculative artifacts and their excerpts.
Treat unchanged cited excerpts as verified: for read-only analysis, call tools only when a
required claim is absent or contradictory, and request narrow line ranges. For edits, verify
only files you will change. Report the result, changed files, and tests concisely; unless the
user asks for detail, stay under 600 English words or 1,200 Chinese characters. Never claim a
tool action that did not occur."""

    def __init__(
        self,
        model: ModelBackend,
        dispatcher: Dispatcher,
        *,
        max_turns: int,
        max_tool_result_chars: int,
    ):
        self.loop = AgentLoop(
            model,
            dispatcher,
            max_turns=max_turns,
            finalize_on_limit=True,
            max_tool_result_chars=max_tool_result_chars,
        )

    async def run(
        self,
        request: str,
        artifacts: list[Artifact],
        *,
        on_tool_result: ToolResultCallback | None = None,
    ) -> str:
        evidence = json.dumps(
            [_artifact_payload(artifact) for artifact in artifacts], ensure_ascii=False, indent=2
        )
        prompt = f"User request:\n{request}\n\nSelected speculative artifacts:\n{evidence}"
        return await self.loop.run(
            system=self.SYSTEM,
            prompt=prompt,
            on_tool_result=on_tool_result,
        )


def artifact_reader(buffer: SpeculativeBuffer) -> Tool:
    def read_artifact(artifact_id: str) -> str:
        return buffer.get(artifact_id).model_dump_json(indent=2)

    return Tool(
        "read_artifact",
        "Read a prior speculative artifact by its exact ID.",
        {
            "type": "object",
            "properties": {"artifact_id": {"type": "string"}},
            "required": ["artifact_id"],
        },
        read_artifact,
    )

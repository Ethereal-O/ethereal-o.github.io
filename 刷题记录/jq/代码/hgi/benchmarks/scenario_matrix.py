from __future__ import annotations

import argparse
import asyncio
import json
import tempfile
import time
from collections.abc import Awaitable, Callable
from pathlib import Path
from typing import Any

from hgi.buffer import SpeculativeBuffer
from hgi.config import HGIConfig, WritePolicy
from hgi.controller import Controller
from hgi.model import OpenAIResponsesBackend
from hgi.schemas import Artifact, Evidence, ModelReply


INPUT_PRICE = 0.20 / 1_000_000
CACHED_INPUT_PRICE = 0.02 / 1_000_000
OUTPUT_PRICE = 1.20 / 1_000_000


def write_fixture(root: Path) -> None:
    files = {
        "services/auth.py": """def validate_token(token: str, expected: str) -> bool:
    return token.startswith(expected)


def authorize(token: str, expected: str) -> str:
    if not validate_token(token, expected):
        raise PermissionError(\"invalid token\")
    return \"allowed\"
""",
        "services/cache.py": """DEFAULT_TTL = 300


def cache_key(namespace: str, item_id: str) -> str:
    return f\"{namespace}:{item_id}\"
""",
        "shared/settings.py": """API_TIMEOUT_SECONDS = 5
WORKER_RETRIES = 3
DEFAULT_TTL = 300
""",
        "packages/api/router.py": """from shared.settings import API_TIMEOUT_SECONDS, DEFAULT_TTL


def request_policy() -> dict[str, int]:
    return {\"timeout\": API_TIMEOUT_SECONDS, \"ttl\": DEFAULT_TTL}
""",
        "packages/worker/jobs.py": """from shared.settings import DEFAULT_TTL, WORKER_RETRIES


def job_policy() -> dict[str, int]:
    return {\"retries\": WORKER_RETRIES, \"ttl\": DEFAULT_TTL}
""",
        "tests/test_auth.py": """from services.auth import validate_token


def test_token_requires_exact_match():
    assert validate_token(\"secret\", \"secret\")
    assert not validate_token(\"secret-extra\", \"secret\")
""",
    }
    for relative, content in files.items():
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
    for index in range(40):
        target = root / "packages" / f"service_{index:02d}" / "module.py"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(
            f"SERVICE_ID = {index}\n\ndef health() -> str:\n    return 'ok-{index}'\n",
            encoding="utf-8",
        )
    large_lines = [f"ROW_{index:04d} = '{'x' * 180}'" for index in range(900)]
    large_lines[777] = "TARGET_POLICY = 'retry only idempotent operations'"
    (root / "large_module.py").write_text("\n".join(large_lines) + "\n", encoding="utf-8")


def read_events(state_dir: Path) -> list[dict[str, Any]]:
    path = state_dir / "logs" / "hgi.jsonl"
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def price(usage: dict[str, int]) -> float:
    input_tokens = usage.get("input_tokens", 0)
    cached_tokens = usage.get("cached_input_tokens", 0)
    output_tokens = usage.get("output_tokens", 0)
    return (
        max(input_tokens - cached_tokens, 0) * INPUT_PRICE
        + cached_tokens * CACHED_INPUT_PRICE
        + output_tokens * OUTPUT_PRICE
    )


def run_records(state_dir: Path) -> list[dict[str, Any]]:
    records = []
    for event in read_events(state_dir):
        if event.get("event") != "run.completed":
            continue
        usage = event.get("usage", {})
        records.append(
            {
                "duration_ms": event.get("duration_ms", 0),
                "answer_chars": event.get("answer_chars", 0),
                "artifact_count": event.get("artifact_count", 0),
                "usage": usage,
                "usage_by_component": event.get("usage_by_component", {}),
                "metrics": event.get("metrics", {}),
                "estimated_cost_usd": round(price(usage), 8),
            }
        )
    return records


def event_counts(state_dir: Path) -> dict[str, int]:
    counts: dict[str, int] = {}
    for event in read_events(state_dir):
        name = str(event.get("event"))
        component = event.get("component")
        key = f"{name}:{component}" if component else name
        counts[key] = counts.get(key, 0) + 1
    return counts


def config(workspace: Path, state_dir: Path, writes: WritePolicy) -> HGIConfig:
    return HGIConfig(
        workspace=workspace,
        state_dir=state_dir,
        writes=writes,
        pause_ms=650,
        min_judge_interval_ms=0,
        summary_checkpoint_chars=600,
        max_agent_turns=8,
        explorer_soft_turns=3,
        max_explorer_turns=5,
        artifact_refresh_timeout_seconds=15,
        artifact_refresh_retries=0,
    )


async def wait_for_exploration(controller: Controller, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if controller._partial_artifact and controller._partial_artifact.findings:
            return
        if controller._explorer_task and controller._explorer_task.done():
            return
        await asyncio.sleep(0.2)


async def close_after_refresh(controller: Controller, timeout: float = 18.0) -> None:
    if controller._refresh_task and not controller._refresh_task.done():
        try:
            await asyncio.wait_for(asyncio.shield(controller._refresh_task), timeout=timeout)
        except TimeoutError:
            controller._refresh_task.cancel()
    await controller.close()


async def scenario_continuous_debug(
    root: Path, backend: OpenAIResponsesBackend
) -> dict[str, Any]:
    workspace = root / "workspace"
    state = root / "state"
    write_fixture(workspace)
    controller = Controller(config(workspace, state, WritePolicy.ALLOW), backend)
    first = (
        "只读定位 tests/test_auth.py 失败的根因，重点检查 services/auth.py，"
        "不要修改文件，简短回答。"
    )
    controller._start_explorer(first)
    await wait_for_exploration(controller)
    first_answer = await controller.commit(first)
    second = (
        "修复刚才发现的 token 前缀误判，只改必要文件，并运行针对性的 pytest。"
        "不要改测试。"
    )
    second_answer = await controller.commit(second)
    await close_after_refresh(controller)
    source = (workspace / "services/auth.py").read_text(encoding="utf-8")
    return {
        "runs": run_records(state),
        "fixed": "startswith" not in source,
        "answers": [first_answer[:240], second_answer[:240]],
        "artifact_files": len(list((state / "artifacts").glob("*.json"))),
        "events": event_counts(state),
    }


async def scenario_similar_different_intent(
    root: Path, backend: OpenAIResponsesBackend
) -> dict[str, Any]:
    workspace = root / "workspace"
    state = root / "state"
    write_fixture(workspace)
    controller = Controller(config(workspace, state, WritePolicy.DENY), backend)
    security = "分析 services/auth.py 的 token 校验安全性，不修改代码，用三点回答。"
    controller._start_explorer(security)
    await wait_for_exploration(controller)
    first_answer = await controller.commit(security)
    performance = (
        "分析 services/auth.py 在每秒十万次调用下的性能开销，只讨论性能，"
        "不要沿用安全结论，不修改代码。"
    )
    second_answer = await controller.commit(performance)
    await controller.close()
    return {
        "runs": run_records(state),
        "answers": [first_answer[:240], second_answer[:240]],
        "artifact_files": len(list((state / "artifacts").glob("*.json"))),
        "events": event_counts(state),
    }


async def scenario_intent_reversal(
    root: Path, backend: OpenAIResponsesBackend
) -> dict[str, Any]:
    workspace = root / "workspace"
    state = root / "state"
    write_fixture(workspace)
    cache_before = (workspace / "services/cache.py").read_text(encoding="utf-8")
    controller = Controller(config(workspace, state, WritePolicy.DENY), backend)
    initial = "请设计用 Redis 替换 services/cache.py 的本地缓存，并分析需要修改的文件。"
    controller.update_draft(initial)
    controller._start_explorer(initial)
    await wait_for_exploration(controller, timeout=6)
    first_had_evidence = bool(
        controller._partial_artifact and controller._partial_artifact.findings
    )
    reversed_request = "不要使用 Redis，也不要修改代码；分析当前本地缓存的 key 和 TTL 设计。"
    controller.update_draft(reversed_request)
    explorer_restarted = controller._start_explorer(reversed_request)
    await wait_for_exploration(controller, timeout=6)
    answer = await controller.commit(reversed_request)
    await controller.close()
    cache_after = (workspace / "services/cache.py").read_text(encoding="utf-8")
    return {
        "runs": run_records(state),
        "answer": answer[:400],
        "first_had_evidence": first_had_evidence,
        "explorer_restarted": explorer_restarted,
        "respected_reversal": cache_after == cache_before
        and "services/cache.py" in answer
        and any(marker in answer for marker in ("本地缓存", "Key", "key", "TTL")),
        "events": event_counts(state),
    }


async def scenario_short_request(
    root: Path, backend: OpenAIResponsesBackend
) -> dict[str, Any]:
    workspace = root / "workspace"
    write_fixture(workspace)
    request = "services/cache.py 的 DEFAULT_TTL 是多少？只回答数值。"
    state_hgi = root / "state-hgi"
    with_hgi = Controller(config(workspace, state_hgi, WritePolicy.DENY), backend)
    with_hgi.update_draft(request)
    await asyncio.sleep(1.2)
    started = time.perf_counter()
    answer_hgi = await with_hgi.commit(request)
    enter_hgi_ms = (time.perf_counter() - started) * 1000
    await with_hgi.close()

    state_direct = root / "state-direct"
    direct = Controller(config(workspace, state_direct, WritePolicy.DENY), backend)
    started = time.perf_counter()
    answer_direct = await direct.commit(request)
    enter_direct_ms = (time.perf_counter() - started) * 1000
    await direct.close()
    return {
        "with_hgi": run_records(state_hgi),
        "direct": run_records(state_direct),
        "measured_enter_ms": {
            "with_hgi": round(enter_hgi_ms, 3),
            "direct": round(enter_direct_ms, 3),
        },
        "answers": [answer_hgi[:120], answer_direct[:120]],
        "hgi_events": event_counts(state_hgi),
    }


async def scenario_monorepo(root: Path, backend: OpenAIResponsesBackend) -> dict[str, Any]:
    workspace = root / "workspace"
    state = root / "state"
    write_fixture(workspace)
    controller = Controller(config(workspace, state, WritePolicy.DENY), backend)
    request = (
        "在这个 monorepo 中追踪 shared/settings.py 的 DEFAULT_TTL 如何分别进入 "
        "packages/api/router.py 与 packages/worker/jobs.py，列出准确路径，不修改代码。"
    )
    controller._start_explorer(request)
    await wait_for_exploration(controller, timeout=10)
    answer = await controller.commit(request)
    await controller.close()
    return {
        "runs": run_records(state),
        "answer": answer[:400],
        "mentions_both_consumers": all(
            item in answer for item in ("packages/api/router.py", "packages/worker/jobs.py")
        ),
        "events": event_counts(state),
    }


async def scenario_large_output(root: Path, backend: OpenAIResponsesBackend) -> dict[str, Any]:
    workspace = root / "workspace"
    write_fixture(workspace)
    broad_state = root / "state-broad"
    broad = Controller(config(workspace, broad_state, WritePolicy.DENY), backend)
    broad_request = (
        "必须先用 read_file 一次读取 large_module.py 第 1 到 400 行，不要使用 grep；"
        "然后找出 TARGET_POLICY 的精确值和行号。若上下文被截断，请如实说明，不修改代码。"
    )
    broad_answer = await broad.commit(broad_request)
    await broad.close()

    narrow_state = root / "state-narrow"
    narrow = Controller(config(workspace, narrow_state, WritePolicy.DENY), backend)
    narrow_request = (
        "读取 large_module.py 并找出 TARGET_POLICY 的精确值。文件很大，"
        "请使用窄查询，最终只返回值和行号，不修改代码。"
    )
    narrow_answer = await narrow.commit(narrow_request)
    await narrow.close()
    return {
        "broad": {
            "runs": run_records(broad_state),
            "answer": broad_answer[:240],
            "correct": "retry only idempotent operations" in broad_answer
            and "778" in broad_answer,
            "events": event_counts(broad_state),
        },
        "narrow": {
            "runs": run_records(narrow_state),
            "answer": narrow_answer[:240],
            "correct": "retry only idempotent operations" in narrow_answer
            and "778" in narrow_answer,
            "events": event_counts(narrow_state),
        },
    }


async def scenario_external_modification(root: Path) -> dict[str, Any]:
    workspace = root / "workspace"
    state = root / "state"
    write_fixture(workspace)
    buffer = SpeculativeBuffer(state, workspace=workspace)
    artifact = Artifact(
        id="Aexternal",
        generation=1,
        goal="inspect authentication token validation",
        summary="token validation uses a prefix comparison",
        findings=[
            Evidence(
                path="services/auth.py",
                line=2,
                claim="validation uses startswith",
                excerpt="2:     return token.startswith(expected)",
            )
        ],
        confidence=0.9,
    )
    persisted = buffer.put(artifact)
    (workspace / "services/auth.py").write_text(
        "def validate_token(token: str, expected: str) -> bool:\n    return token == expected\n",
        encoding="utf-8",
    )
    selected = buffer.search("inspect authentication token validation", include_stale=True)
    validated = buffer.get("Aexternal")
    return {
        "persisted": persisted,
        "reusable_finding_count": sum(len(item.findings) for item in selected),
        "stale_unresolved": validated.unresolved,
        "passed": not validated.findings and bool(validated.unresolved),
    }


class FailureModel:
    model = "deterministic-failure-model"

    async def generate(self, *, system: str, messages: Any, tools: Any = ()) -> ModelReply:
        if "input-boundary judge" in system:
            raise RuntimeError("injected judge failure")
        if "Maintain a concise summary" in system:
            raise RuntimeError("injected summary failure")
        if "read-only coding repository explorer" in system:
            raise RuntimeError("injected explorer failure")
        if "committed coding agent" in system:
            return ModelReply(text="fallback commit completed")
        raise AssertionError(system)


async def scenario_failures(root: Path) -> dict[str, Any]:
    workspace = root / "workspace"
    state = root / "state"
    write_fixture(workspace)
    controller = Controller(config(workspace, state, WritePolicy.DENY), FailureModel())
    request = "分析 services/auth.py。"
    controller.update_draft(request)
    await asyncio.sleep(0.8)
    await controller._update_summary()
    controller._start_explorer(request)
    if controller._explorer_task:
        await controller._explorer_task
    answer = await controller.commit(request)
    await controller.close()
    counts = event_counts(state)
    return {
        "answer": answer,
        "commit_survived": answer == "fallback commit completed",
        "failed_calls": sum(
            value for key, value in counts.items() if key.startswith("model.call.failed")
        ),
        "events": counts,
    }


async def scenario_cross_topic(root: Path, backend: OpenAIResponsesBackend) -> dict[str, Any]:
    workspace = root / "workspace"
    state = root / "state"
    write_fixture(workspace)
    controller = Controller(config(workspace, state, WritePolicy.DENY), backend)
    api_request = (
        "分析 packages/api/router.py 的超时配置来源，同时记录 shared/settings.py 的相关证据，"
        "不修改代码。"
    )
    controller._start_explorer(api_request)
    await wait_for_exploration(controller)
    first = await controller.commit(api_request)
    worker_request = (
        "分析 packages/worker/jobs.py 的重试次数来源，以及它和 shared/settings.py 的关系，"
        "不修改代码。"
    )
    controller._start_explorer(worker_request)
    await wait_for_exploration(controller)
    second = await controller.commit(worker_request)
    await controller.close()
    indexes = SpeculativeBuffer(state, workspace=workspace).indexes()
    return {
        "runs": run_records(state),
        "answers": [first[:240], second[:240]],
        "artifact_count": len(indexes),
        "artifact_paths": [index.paths for index in indexes],
        "events": event_counts(state),
    }


async def scenario_answer_length(
    root: Path, backend: OpenAIResponsesBackend
) -> dict[str, Any]:
    workspace = root / "workspace"
    write_fixture(workspace)
    prompts = {
        "short": (
            "解释 services/auth.py 的 validate_token 当前行为，只用一句中文，"
            "不要列举，不修改代码。"
        ),
        "detailed": (
            "详细解释 services/auth.py 的 validate_token 当前行为、调用路径、失败模式和修复建议，"
            "使用分节说明并引用准确路径，不修改代码。"
        ),
    }
    result: dict[str, Any] = {}
    for label, prompt in prompts.items():
        state = root / f"state-{label}"
        controller = Controller(config(workspace, state, WritePolicy.DENY), backend)
        answer = await controller.commit(prompt)
        await controller.close()
        result[label] = {"runs": run_records(state), "answer": answer[:400]}
    return result


async def execute(name: str, operation: Callable[[], Awaitable[dict[str, Any]]]) -> dict[str, Any]:
    print(f"START {name}", flush=True)
    started = time.perf_counter()
    try:
        result = await operation()
        result["wall_ms"] = round((time.perf_counter() - started) * 1000, 3)
        result["status"] = "passed"
    except Exception as exc:
        result = {
            "status": "failed",
            "error_type": type(exc).__name__,
            "error": str(exc)[:500],
            "wall_ms": round((time.perf_counter() - started) * 1000, 3),
        }
    print(f"DONE {name} {result['status']} {result['wall_ms']}ms", flush=True)
    return result


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    parser.add_argument("--model", default="gpt-5.6-luna")
    parser.add_argument("--reasoning-effort", default="low")
    parser.add_argument("--max-output-tokens", type=int, default=1200)
    parser.add_argument("--scenario", action="append", dest="selected")
    args = parser.parse_args()
    run_root = Path(tempfile.mkdtemp(prefix="hgi-scenario-matrix-"))
    backend = OpenAIResponsesBackend.from_codex_config(
        model=args.model,
        reasoning_effort=args.reasoning_effort,
        max_output_tokens=args.max_output_tokens,
    )
    scenarios: list[tuple[str, Callable[[], Awaitable[dict[str, Any]]]]] = [
        ("continuous_debug", lambda: scenario_continuous_debug(run_root / "01", backend)),
        (
            "similar_different_intent",
            lambda: scenario_similar_different_intent(run_root / "02", backend),
        ),
        ("intent_reversal", lambda: scenario_intent_reversal(run_root / "03", backend)),
        ("short_request", lambda: scenario_short_request(run_root / "04", backend)),
        ("monorepo", lambda: scenario_monorepo(run_root / "05", backend)),
        ("large_output", lambda: scenario_large_output(run_root / "06", backend)),
        ("external_modification", lambda: scenario_external_modification(run_root / "07")),
        ("service_failures", lambda: scenario_failures(run_root / "08")),
        ("cross_topic", lambda: scenario_cross_topic(run_root / "09", backend)),
        ("answer_length", lambda: scenario_answer_length(run_root / "10", backend)),
    ]
    results: dict[str, Any] = {
        "configuration": {
            "model": args.model,
            "reasoning_effort": args.reasoning_effort,
            "max_output_tokens": args.max_output_tokens,
            "run_root": str(run_root),
        },
        "scenarios": {},
    }
    try:
        for name, operation in scenarios:
            if args.selected and name not in args.selected:
                continue
            results["scenarios"][name] = await execute(name, operation)
    finally:
        await backend.client.close()
    output = args.output or (run_root / "results.json")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"RESULTS {output}", flush=True)


if __name__ == "__main__":
    asyncio.run(main())

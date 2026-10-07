from __future__ import annotations

import argparse
import asyncio
import json
import tempfile
import time
from collections.abc import Awaitable, Callable
from pathlib import Path
from typing import Any

from hgi.config import WritePolicy
from hgi.controller import Controller
from hgi.model import OpenAIResponsesBackend
from hgi.schemas import ModelReply

from scenario_matrix import config, event_counts, price, run_records, write_fixture


def controller_config(
    workspace: Path,
    state_dir: Path,
    writes: WritePolicy,
    *,
    speculate: bool,
):
    value = config(workspace, state_dir, writes)
    if not speculate:
        value.auto_explore = False
        value.artifact_limit = 0
    return value


async def commit_once(
    controller: Controller,
    request: str,
    *,
    speculate: bool,
    window_seconds: float,
) -> tuple[str, float, float]:
    input_started = time.perf_counter()
    if speculate:
        controller._start_explorer(request)
        await asyncio.sleep(window_seconds)
    enter_started = time.perf_counter()
    answer = await controller.commit(request)
    answered = time.perf_counter()
    return (
        answer,
        round((answered - enter_started) * 1000, 3),
        round((answered - input_started) * 1000, 3),
    )


def summarize_variant(
    state: Path,
    answers: list[str],
    enter_ms: list[float],
    total_ms: list[float],
    checks: dict[str, bool],
) -> dict[str, Any]:
    runs = run_records(state)
    usage_fields = (
        "input_tokens",
        "cached_input_tokens",
        "output_tokens",
        "reasoning_tokens",
        "total_tokens",
    )
    usage = {
        field: sum(record["usage"].get(field, 0) for record in runs)
        for field in usage_fields
    }
    return {
        "runs": runs,
        "enter_ms": enter_ms,
        "total_ms": total_ms,
        "enter_ms_sum": round(sum(enter_ms), 3),
        "total_ms_sum": round(sum(total_ms), 3),
        "usage": usage,
        "estimated_cost_usd": round(price(usage), 8),
        "answers": [answer[:400] for answer in answers],
        "checks": checks,
        "events": event_counts(state),
    }


def comparison(baseline: dict[str, Any], speculate: dict[str, Any]) -> dict[str, Any]:
    def delta_percent(current: float, original: float) -> float | None:
        if not original:
            return None
        return round((current - original) / original * 100, 1)

    return {
        "enter_ms_delta_pct": delta_percent(
            speculate["enter_ms_sum"], baseline["enter_ms_sum"]
        ),
        "total_ms_delta_pct": delta_percent(
            speculate["total_ms_sum"], baseline["total_ms_sum"]
        ),
        "tokens_delta_pct": delta_percent(
            speculate["usage"]["total_tokens"], baseline["usage"]["total_tokens"]
        ),
        "cost_delta_pct": delta_percent(
            speculate["estimated_cost_usd"], baseline["estimated_cost_usd"]
        ),
    }


async def paired_continuous_debug(
    root: Path, backend: OpenAIResponsesBackend, window: float
) -> dict[str, Any]:
    variants = {}
    requests = [
        "只读定位 tests/test_auth.py 失败的根因，重点检查 services/auth.py；两句话回答。",
        "修复 token 前缀误判，只改必要文件，运行 pytest tests/test_auth.py；简短回答。",
    ]
    for speculate in (False, True):
        label = "speculate" if speculate else "baseline"
        workspace = root / label / "workspace"
        state = root / label / "state"
        write_fixture(workspace)
        controller = Controller(
            controller_config(workspace, state, WritePolicy.ALLOW, speculate=speculate),
            backend,
        )
        answers, enters, totals = [], [], []
        for request in requests:
            answer, enter_ms, total_ms = await commit_once(
                controller,
                request,
                speculate=speculate,
                window_seconds=window,
            )
            answers.append(answer)
            enters.append(enter_ms)
            totals.append(total_ms)
        await controller.close()
        source = (workspace / "services/auth.py").read_text(encoding="utf-8")
        variants[label] = summarize_variant(
            state,
            answers,
            enters,
            totals,
            {
                "diagnosed": "startswith" in answers[0],
                "fixed": "startswith" not in source,
                "test_passed": "passed" in answers[1].lower(),
            },
        )
    variants["comparison"] = comparison(variants["baseline"], variants["speculate"])
    return variants


async def paired_two_intents(
    root: Path, backend: OpenAIResponsesBackend, window: float
) -> dict[str, Any]:
    variants = {}
    requests = [
        "分析 services/auth.py 的 token 校验安全性，不修改代码；两句话回答。",
        "分析 services/auth.py 在高频调用下的性能，不讨论安全性，不修改代码；两句话回答。",
    ]
    for speculate in (False, True):
        label = "speculate" if speculate else "baseline"
        workspace = root / label / "workspace"
        state = root / label / "state"
        write_fixture(workspace)
        controller = Controller(
            controller_config(workspace, state, WritePolicy.DENY, speculate=speculate),
            backend,
        )
        answers, enters, totals = [], [], []
        for request in requests:
            answer, enter_ms, total_ms = await commit_once(
                controller, request, speculate=speculate, window_seconds=window
            )
            answers.append(answer)
            enters.append(enter_ms)
            totals.append(total_ms)
        await controller.close()
        variants[label] = summarize_variant(
            state,
            answers,
            enters,
            totals,
            {
                "security_answered": "startswith" in answers[0],
                "performance_answered": any(
                    marker in answers[1].lower()
                    for marker in ("performance", "性能", "复杂度", "开销", "o(")
                ),
            },
        )
    variants["comparison"] = comparison(variants["baseline"], variants["speculate"])
    return variants


async def paired_intent_reversal(
    root: Path, backend: OpenAIResponsesBackend, window: float
) -> dict[str, Any]:
    final_request = "不要使用 Redis，也不要修改代码；分析 services/cache.py 的 key 和 TTL 设计。"
    variants = {}
    for speculate in (False, True):
        label = "speculate" if speculate else "baseline"
        workspace = root / label / "workspace"
        state = root / label / "state"
        write_fixture(workspace)
        before = (workspace / "services/cache.py").read_text(encoding="utf-8")
        controller = Controller(
            controller_config(workspace, state, WritePolicy.DENY, speculate=speculate),
            backend,
        )
        started = time.perf_counter()
        restarted = False
        if speculate:
            controller._start_explorer(
                "请设计用 Redis 替换 services/cache.py 的本地缓存，并分析修改范围。"
            )
            await asyncio.sleep(window / 2)
            restarted = controller._start_explorer(final_request)
            await asyncio.sleep(window / 2)
        enter_started = time.perf_counter()
        answer = await controller.commit(final_request)
        answered = time.perf_counter()
        await controller.close()
        after = (workspace / "services/cache.py").read_text(encoding="utf-8")
        variants[label] = summarize_variant(
            state,
            [answer],
            [round((answered - enter_started) * 1000, 3)],
            [round((answered - started) * 1000, 3)],
            {
                "unchanged": before == after,
                "local_cache_answered": "TTL" in answer and "cache.py" in answer,
                "explorer_restarted": restarted if speculate else True,
            },
        )
    variants["comparison"] = comparison(variants["baseline"], variants["speculate"])
    return variants


async def paired_single_request(
    root: Path,
    backend: OpenAIResponsesBackend,
    window: float,
    request: str,
    check: Callable[[str], bool],
) -> dict[str, Any]:
    variants = {}
    for speculate in (False, True):
        label = "speculate" if speculate else "baseline"
        workspace = root / label / "workspace"
        state = root / label / "state"
        write_fixture(workspace)
        controller = Controller(
            controller_config(workspace, state, WritePolicy.DENY, speculate=speculate),
            backend,
        )
        answer, enter_ms, total_ms = await commit_once(
            controller, request, speculate=speculate, window_seconds=window
        )
        await controller.close()
        variants[label] = summarize_variant(
            state, [answer], [enter_ms], [total_ms], {"correct": check(answer)}
        )
    variants["comparison"] = comparison(variants["baseline"], variants["speculate"])
    return variants


async def paired_external_modification(
    root: Path, backend: OpenAIResponsesBackend, window: float
) -> dict[str, Any]:
    variants = {}
    request = "说明 services/auth.py 当前如何校验 token，不修改代码；一句话回答。"
    for speculate in (False, True):
        label = "speculate" if speculate else "baseline"
        workspace = root / label / "workspace"
        state = root / label / "state"
        write_fixture(workspace)
        controller = Controller(
            controller_config(workspace, state, WritePolicy.DENY, speculate=speculate),
            backend,
        )
        first, first_enter, first_total = await commit_once(
            controller, request, speculate=speculate, window_seconds=window
        )
        (workspace / "services/auth.py").write_text(
            "def validate_token(token: str, expected: str) -> bool:\n    return token == expected\n",
            encoding="utf-8",
        )
        second, second_enter, second_total = await commit_once(
            controller, request, speculate=speculate, window_seconds=window
        )
        await controller.close()
        variants[label] = summarize_variant(
            state,
            [first, second],
            [first_enter, second_enter],
            [first_total, second_total],
            {
                "old_seen_first": "startswith" in first,
                "new_seen_second": any(marker in second for marker in ("==", "相等", "完全一致")),
            },
        )
    variants["comparison"] = comparison(variants["baseline"], variants["speculate"])
    return variants


class FailureABModel:
    model = "deterministic-failure-ab"

    def __init__(self, fail_explorer: bool):
        self.fail_explorer = fail_explorer

    async def generate(self, *, system: str, messages: Any, tools: Any = ()) -> ModelReply:
        if "read-only coding repository explorer" in system and self.fail_explorer:
            raise RuntimeError("injected explorer failure")
        if "committed coding agent" in system:
            return ModelReply(text="fallback commit completed")
        if "read-only coding repository explorer" in system:
            return ModelReply(
                text='{"summary":"no evidence","findings":[],"unresolved":[],"confidence":0.1}'
            )
        raise AssertionError(system)


async def paired_service_failure(root: Path, window: float) -> dict[str, Any]:
    variants = {}
    for speculate in (False, True):
        label = "speculate" if speculate else "baseline"
        workspace = root / label / "workspace"
        state = root / label / "state"
        write_fixture(workspace)
        controller = Controller(
            controller_config(workspace, state, WritePolicy.DENY, speculate=speculate),
            FailureABModel(fail_explorer=speculate),
        )
        answer, enter_ms, total_ms = await commit_once(
            controller,
            "分析 services/auth.py。",
            speculate=speculate,
            window_seconds=min(window, 0.2),
        )
        await controller.close()
        variants[label] = summarize_variant(
            state,
            [answer],
            [enter_ms],
            [total_ms],
            {"commit_survived": answer == "fallback commit completed"},
        )
    variants["comparison"] = comparison(variants["baseline"], variants["speculate"])
    return variants


async def paired_cross_topic(
    root: Path, backend: OpenAIResponsesBackend, window: float
) -> dict[str, Any]:
    variants = {}
    requests = [
        "说明 packages/api/router.py 的超时配置来源，不修改代码；两句话回答。",
        "说明 packages/worker/jobs.py 的重试次数来源，不修改代码；两句话回答。",
    ]
    for speculate in (False, True):
        label = "speculate" if speculate else "baseline"
        workspace = root / label / "workspace"
        state = root / label / "state"
        write_fixture(workspace)
        controller = Controller(
            controller_config(workspace, state, WritePolicy.DENY, speculate=speculate),
            backend,
        )
        answers, enters, totals = [], [], []
        for request in requests:
            answer, enter_ms, total_ms = await commit_once(
                controller, request, speculate=speculate, window_seconds=window
            )
            answers.append(answer)
            enters.append(enter_ms)
            totals.append(total_ms)
        await controller.close()
        variants[label] = summarize_variant(
            state,
            answers,
            enters,
            totals,
            {
                "api_correct": "API_TIMEOUT_SECONDS" in answers[0],
                "worker_correct": "WORKER_RETRIES" in answers[1],
            },
        )
    variants["comparison"] = comparison(variants["baseline"], variants["speculate"])
    return variants


async def execute(name: str, operation: Callable[[], Awaitable[dict[str, Any]]]) -> dict[str, Any]:
    print(f"START {name}", flush=True)
    started = time.perf_counter()
    try:
        result = await operation()
        result["status"] = "passed"
    except Exception as exc:
        result = {
            "status": "failed",
            "error_type": type(exc).__name__,
            "error": str(exc)[:500],
        }
    result["wall_ms"] = round((time.perf_counter() - started) * 1000, 3)
    print(f"DONE {name} {result['status']} {result['wall_ms']}ms", flush=True)
    return result


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    parser.add_argument("--model", default="gpt-5.6-luna")
    parser.add_argument("--reasoning-effort", default="low")
    parser.add_argument("--max-output-tokens", type=int, default=1200)
    parser.add_argument("--window-seconds", type=float, default=6.0)
    parser.add_argument("--scenario", action="append", dest="selected")
    args = parser.parse_args()
    run_root = Path(tempfile.mkdtemp(prefix="hgi-speculation-ab-"))
    backend = OpenAIResponsesBackend.from_codex_config(
        model=args.model,
        reasoning_effort=args.reasoning_effort,
        max_output_tokens=args.max_output_tokens,
    )
    window = args.window_seconds
    jobs: list[tuple[str, Callable[[], Awaitable[dict[str, Any]]]]] = [
        (
            "continuous_debug",
            lambda: paired_continuous_debug(run_root / "01", backend, window),
        ),
        (
            "similar_different_intent",
            lambda: paired_two_intents(run_root / "02", backend, window),
        ),
        (
            "intent_reversal",
            lambda: paired_intent_reversal(run_root / "03", backend, window),
        ),
        (
            "short_request",
            lambda: paired_single_request(
                run_root / "04",
                backend,
                1.0,
                "services/cache.py 的 DEFAULT_TTL 是多少？只回答数值。",
                lambda answer: "300" in answer,
            ),
        ),
        (
            "monorepo",
            lambda: paired_single_request(
                run_root / "05",
                backend,
                window,
                "追踪 shared/settings.py 的 DEFAULT_TTL 如何进入 packages/api/router.py 和 "
                "packages/worker/jobs.py；列出路径，不修改代码。",
                lambda answer: "packages/api/router.py" in answer
                and "packages/worker/jobs.py" in answer,
            ),
        ),
        (
            "large_output",
            lambda: paired_single_request(
                run_root / "06",
                backend,
                window,
                "在 large_module.py 中找到 TARGET_POLICY 的精确值和行号；使用窄查询，"
                "只返回值与行号，不修改代码。",
                lambda answer: "retry only idempotent operations" in answer
                and "778" in answer,
            ),
        ),
        (
            "external_modification",
            lambda: paired_external_modification(run_root / "07", backend, window),
        ),
        (
            "service_failure",
            lambda: paired_service_failure(run_root / "08", window),
        ),
        (
            "cross_topic",
            lambda: paired_cross_topic(run_root / "09", backend, window),
        ),
        (
            "answer_short",
            lambda: paired_single_request(
                run_root / "10-short",
                backend,
                window,
                "解释 services/auth.py 的 validate_token 当前行为，只用一句中文，不修改代码。",
                lambda answer: "startswith" in answer or "开头" in answer,
            ),
        ),
        (
            "answer_detailed",
            lambda: paired_single_request(
                run_root / "10-detailed",
                backend,
                window,
                "详细解释 services/auth.py 的 validate_token 行为、调用路径、失败模式和修复建议，"
                "分节并引用路径，不修改代码。",
                lambda answer: "startswith" in answer and "authorize" in answer,
            ),
        ),
    ]
    results: dict[str, Any] = {
        "configuration": {
            "model": args.model,
            "reasoning_effort": args.reasoning_effort,
            "max_output_tokens": args.max_output_tokens,
            "window_seconds": window,
            "baseline": "no Explorer and artifact_limit=0",
            "speculate": "fixed pre-commit Explorer window and artifact_limit=4",
            "run_root": str(run_root),
        },
        "scenarios": {},
    }
    try:
        for name, operation in jobs:
            if args.selected and name not in args.selected:
                continue
            results["scenarios"][name] = await execute(name, operation)
    finally:
        await backend.client.close()
    output = args.output or run_root / "results.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"RESULTS {output}", flush=True)


if __name__ == "__main__":
    asyncio.run(main())

import asyncio
import json

import pytest

from hgi.observability import InstrumentedModelBackend, StructuredLogger, log_context
from hgi.schemas import ModelReply, ModelUsage


def read_events(path):
    return [json.loads(line) for line in path.read_text().splitlines()]


def test_structured_logger_redacts_and_preserves_context(tmp_path):
    path = tmp_path / "hgi.jsonl"
    logger = StructuredLogger(path, session_id="session-1")
    with log_context(run_id="run-1", component="test"):
        logger.log(
            "test.event",
            api_key="secret-key",
            request="private prompt",
            safe_value=42,
            nested={"content": "source code", "count": 3},
        )
    logger.close()

    event = read_events(path)[0]
    assert event["event"] == "test.event"
    assert event["session_id"] == "session-1"
    assert event["run_id"] == "run-1"
    assert event["component"] == "test"
    assert event["api_key"] == "[REDACTED]"
    assert event["request"] == "[REDACTED]"
    assert event["nested"]["content"] == "[REDACTED]"
    assert event["safe_value"] == 42
    assert event["timestamp"].endswith("Z")


def test_structured_logger_rotates_files(tmp_path):
    path = tmp_path / "hgi.jsonl"
    logger = StructuredLogger(path, max_bytes=500, backup_count=2)
    for index in range(30):
        logger.log("rotation.event", index=index, safe_value="x" * 80)
    logger.close()

    assert path.exists()
    assert (tmp_path / "hgi.jsonl.1").exists()


@pytest.mark.asyncio
async def test_instrumented_model_logs_usage_and_aggregates_by_run(tmp_path):
    class FakeModel:
        model = "fake-model"

        async def generate(self, **_kwargs):
            await asyncio.sleep(0)
            return ModelReply(
                text="private response",
                response_id="response-1",
                usage=ModelUsage(
                    input_tokens=10,
                    cached_input_tokens=2,
                    output_tokens=4,
                    reasoning_tokens=3,
                    total_tokens=14,
                ),
            )

    path = tmp_path / "hgi.jsonl"
    logger = StructuredLogger(path)
    model = InstrumentedModelBackend(FakeModel(), logger)
    with log_context(run_id="run-1"):
        await model.generate(system="committed coding agent", messages=[], tools=[])
    usage = logger.usage_for("run-1")
    breakdown = logger.usage_breakdown_for("run-1")
    metrics = logger.metrics_for("run-1")
    logger.close()

    assert usage.total_tokens == 14
    assert usage.reasoning_tokens == 3
    assert breakdown["committed"].input_tokens == 10
    assert metrics["model_calls.committed.completed"] == 1
    completed = [event for event in read_events(path) if event["event"] == "model.call.completed"]
    assert completed[0]["component"] == "committed"
    assert completed[0]["usage"]["input_tokens"] == 10
    assert "private response" not in path.read_text()

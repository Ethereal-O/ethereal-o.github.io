from __future__ import annotations

import asyncio
from collections import Counter
import logging
import queue
import threading
import time
from contextlib import contextmanager
from enum import Enum
from logging.handlers import QueueHandler, QueueListener, RotatingFileHandler
from pathlib import Path
from typing import Any, Iterator, Sequence
from uuid import uuid4

import structlog
from pydantic import BaseModel
from structlog.contextvars import bind_contextvars, get_contextvars, reset_contextvars

from .model import ModelBackend
from .schemas import ModelReply, ModelUsage

_SENSITIVE_KEYS = {
    "api_key",
    "authorization",
    "content",
    "cookie",
    "error",
    "excerpt",
    "exploration_goal",
    "goal",
    "message",
    "password",
    "prompt",
    "reason",
    "request",
    "secret",
    "summary",
    "text",
    "token",
}


@contextmanager
def log_context(**fields: Any) -> Iterator[None]:
    tokens = bind_contextvars(**{key: value for key, value in fields.items() if value is not None})
    try:
        yield
    finally:
        reset_contextvars(**tokens)


def current_log_context() -> dict[str, Any]:
    return get_contextvars()


def _json_value(value: Any) -> Any:
    if isinstance(value, BaseModel):
        return _json_value(value.model_dump(mode="json"))
    if isinstance(value, dict):
        result: dict[str, Any] = {}
        for key, item in value.items():
            lowered = str(key).lower()
            sensitive = lowered in _SENSITIVE_KEYS or any(
                marker in lowered for marker in ("password", "secret", "api_key", "auth_token")
            )
            if sensitive and not isinstance(item, ModelUsage):
                result[str(key)] = "[REDACTED]"
            else:
                result[str(key)] = _json_value(item)
        return result
    if isinstance(value, (list, tuple, set)):
        return [_json_value(item) for item in value]
    if isinstance(value, (Path, Enum)):
        return str(value)
    if isinstance(value, bytes):
        return f"<bytes:{len(value)}>"
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    return repr(value)


def _redact_processor(
    _logger: Any, _method_name: str, event_dict: dict[str, Any]
) -> dict[str, Any]:
    return _json_value(event_dict)


class StructuredLogger:
    """Queued structlog JSONL logger with rotation and usage aggregation."""

    def __init__(
        self,
        path: Path,
        *,
        session_id: str | None = None,
        level: str = "INFO",
        max_bytes: int = 10_000_000,
        backup_count: int = 5,
        enabled: bool = True,
    ):
        self.path = path
        self.session_id = session_id or uuid4().hex
        self.enabled = enabled
        self._closed = False
        self._usage: dict[str, ModelUsage] = {}
        self._usage_by_component: dict[str, dict[str, ModelUsage]] = {}
        self._metrics: dict[str, Counter[str]] = {}
        self._usage_lock = threading.Lock()
        self._stdlib_logger: logging.Logger | None = None
        self._listener: QueueListener | None = None
        self._handler: RotatingFileHandler | None = None
        self._logger: structlog.stdlib.BoundLogger | None = None
        if not enabled:
            return

        path.parent.mkdir(parents=True, exist_ok=True)
        log_queue: queue.SimpleQueue[logging.LogRecord] = queue.SimpleQueue()
        handler = RotatingFileHandler(
            path,
            maxBytes=max_bytes,
            backupCount=backup_count,
            encoding="utf-8",
            delay=True,
        )
        handler.setFormatter(logging.Formatter("%(message)s"))
        stdlib_logger = logging.getLogger(f"hgi.{self.session_id}")
        stdlib_logger.handlers.clear()
        stdlib_logger.addHandler(QueueHandler(log_queue))
        stdlib_logger.setLevel(getattr(logging, level.upper()))
        stdlib_logger.propagate = False
        listener = QueueListener(log_queue, handler, respect_handler_level=True)
        listener.start()
        self._logger = structlog.wrap_logger(
            stdlib_logger,
            processors=[
                structlog.contextvars.merge_contextvars,
                _redact_processor,
                structlog.processors.TimeStamper(fmt="iso", utc=True, key="timestamp"),
                structlog.processors.add_log_level,
                structlog.processors.JSONRenderer(ensure_ascii=False, sort_keys=True),
            ],
            wrapper_class=structlog.stdlib.BoundLogger,
            cache_logger_on_first_use=True,
        ).bind(schema_version=1, session_id=self.session_id)
        self._stdlib_logger = stdlib_logger
        self._listener = listener
        self._handler = handler

    def log(self, event: str, *, level: str = "INFO", **fields: Any) -> None:
        if not self.enabled or self._closed or self._logger is None:
            return
        method = getattr(self._logger, level.lower(), self._logger.info)
        method(event, **fields)

    def add_usage(self, usage: ModelUsage | None, *, component: str = "model") -> None:
        run_id = current_log_context().get("run_id")
        if usage is None or run_id is None:
            return
        with self._usage_lock:
            total = self._usage.setdefault(str(run_id), ModelUsage())
            component_usage = self._usage_by_component.setdefault(str(run_id), {}).setdefault(
                component, ModelUsage()
            )
            for target in (total, component_usage):
                for field in ModelUsage.model_fields:
                    setattr(target, field, getattr(target, field) + getattr(usage, field))

    def usage_for(self, run_id: str, *, clear: bool = False) -> ModelUsage:
        with self._usage_lock:
            usage = self._usage.get(run_id, ModelUsage()).model_copy()
            if clear:
                self._usage.pop(run_id, None)
            return usage

    def usage_breakdown_for(self, run_id: str, *, clear: bool = False) -> dict[str, ModelUsage]:
        with self._usage_lock:
            usage = {
                component: total.model_copy()
                for component, total in self._usage_by_component.get(run_id, {}).items()
            }
            if clear:
                self._usage_by_component.pop(run_id, None)
            return usage

    def increment(self, metric: str, amount: int = 1) -> None:
        run_id = current_log_context().get("run_id")
        if run_id is None:
            return
        with self._usage_lock:
            self._metrics.setdefault(str(run_id), Counter())[metric] += amount

    def metrics_for(self, run_id: str, *, clear: bool = False) -> dict[str, int]:
        with self._usage_lock:
            metrics = dict(self._metrics.get(run_id, Counter()))
            if clear:
                self._metrics.pop(run_id, None)
            return metrics

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self._listener:
            self._listener.stop()
        if self._handler:
            self._handler.close()
        if self._stdlib_logger:
            self._stdlib_logger.handlers.clear()


class InstrumentedModelBackend:
    def __init__(self, backend: ModelBackend, logger: StructuredLogger):
        self.backend = backend
        self.logger = logger
        self.model = getattr(backend, "model", type(backend).__name__)

    async def generate(
        self,
        *,
        system: str,
        messages: Sequence[dict[str, Any]],
        tools: Sequence[dict[str, Any]] = (),
    ) -> ModelReply:
        call_id = uuid4().hex
        phase = self._phase(system)
        started = time.perf_counter()
        self.logger.log(
            "model.call.started",
            call_id=call_id,
            component=phase,
            model=self.model,
            message_count=len(messages),
            tool_count=len(tools),
        )
        self.logger.increment(f"model_calls.{phase}.started")
        try:
            reply = await self.backend.generate(system=system, messages=messages, tools=tools)
        except asyncio.CancelledError:
            self.logger.log(
                "model.call.cancelled",
                level="WARNING",
                call_id=call_id,
                component=phase,
                model=self.model,
                duration_ms=round((time.perf_counter() - started) * 1000, 3),
            )
            self.logger.increment(f"model_calls.{phase}.cancelled")
            raise
        except Exception as exc:
            self.logger.log(
                "model.call.failed",
                level="ERROR",
                call_id=call_id,
                component=phase,
                model=self.model,
                duration_ms=round((time.perf_counter() - started) * 1000, 3),
                error_type=type(exc).__name__,
                error=str(exc),
            )
            self.logger.increment(f"model_calls.{phase}.failed")
            raise
        self.logger.add_usage(reply.usage, component=phase)
        self.logger.increment(f"model_calls.{phase}.completed")
        self.logger.log(
            "model.call.completed",
            call_id=call_id,
            component=phase,
            model=self.model,
            response_id=reply.response_id,
            duration_ms=round((time.perf_counter() - started) * 1000, 3),
            usage=reply.usage,
            output_chars=len(reply.text),
            tool_calls=[call.name for call in reply.tool_calls],
        )
        return reply

    @staticmethod
    def _phase(system: str) -> str:
        markers = (
            ("input-boundary judge", "judge"),
            ("Maintain a concise summary", "summary"),
            ("read-only coding repository explorer", "explorer"),
            ("committed coding agent", "committed"),
        )
        return next((phase for marker, phase in markers if marker in system), "model")

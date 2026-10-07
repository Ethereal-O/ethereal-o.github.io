from __future__ import annotations

from datetime import datetime, timezone
from enum import StrEnum
from typing import Any, Literal

from pydantic import BaseModel, Field


class JudgeAction(StrEnum):
    WAIT = "wait"
    SUMMARIZE_AND_WAIT = "summarize_and_wait"
    EXPLORE = "explore"
    COMMIT = "commit"


class JudgeDecision(BaseModel):
    action: JudgeAction
    exploration_goal: str = ""
    reason: str = ""
    confidence: float = Field(default=0.0, ge=0.0, le=1.0)


class ToolCall(BaseModel):
    id: str
    name: str
    arguments: dict[str, Any] = Field(default_factory=dict)


class ModelUsage(BaseModel):
    input_tokens: int = Field(default=0, ge=0)
    cached_input_tokens: int = Field(default=0, ge=0)
    output_tokens: int = Field(default=0, ge=0)
    reasoning_tokens: int = Field(default=0, ge=0)
    total_tokens: int = Field(default=0, ge=0)


class ModelReply(BaseModel):
    text: str = ""
    tool_calls: list[ToolCall] = Field(default_factory=list)
    usage: ModelUsage | None = None
    response_id: str | None = None

    def assistant_content(self) -> list[dict[str, Any]]:
        content: list[dict[str, Any]] = []
        if self.text:
            content.append({"type": "text", "text": self.text})
        content.extend(
            {
                "type": "tool_use",
                "id": call.id,
                "name": call.name,
                "input": call.arguments,
            }
            for call in self.tool_calls
        )
        return content


class Evidence(BaseModel):
    path: str
    line: int | None = None
    claim: str
    excerpt: str = ""
    content_hash: str | None = None


class Artifact(BaseModel):
    id: str
    generation: int
    goal: str
    summary: str
    findings: list[Evidence] = Field(default_factory=list)
    unresolved: list[str] = Field(default_factory=list)
    confidence: float = Field(default=0.5, ge=0.0, le=1.0)
    provisional: bool = False
    created_at: datetime = Field(default_factory=lambda: datetime.now(timezone.utc))


class ArtifactIndex(BaseModel):
    id: str
    goal: str
    summary: str
    paths: list[str] = Field(default_factory=list)
    confidence: float
    provisional: bool = False
    created_at: datetime


class RunStatus(StrEnum):
    IDLE = "idle"
    RUNNING = "running"
    COMPLETE = "complete"
    FAILED = "failed"
    CANCELLED = "cancelled"


class RuntimeEvent(BaseModel):
    kind: Literal[
        "judge",
        "summary",
        "explorer",
        "committed",
        "recommend_commit",
        "recommend_interrupt",
        "error",
    ]
    status: RunStatus | None = None
    message: str = ""
    data: dict[str, Any] = Field(default_factory=dict)

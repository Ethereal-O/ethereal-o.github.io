from __future__ import annotations

from enum import StrEnum
from pathlib import Path

from pydantic import BaseModel, Field


class ControlMode(StrEnum):
    MANUAL = "manual"
    ASSIST = "assist"
    AUTO = "auto"


class WritePolicy(StrEnum):
    ASK = "ask"
    ALLOW = "allow"
    DENY = "deny"


class HGIConfig(BaseModel):
    workspace: Path = Field(default_factory=Path.cwd)
    state_dir: Path | None = None
    auto_explore: bool = True
    auto_cancel_explorer: bool = True
    commit_mode: ControlMode = ControlMode.MANUAL
    interrupt_mode: ControlMode = ControlMode.MANUAL
    judge_shadow_control: bool = True
    writes: WritePolicy = WritePolicy.ASK
    pause_ms: int = Field(default=650, ge=100)
    min_judge_interval_ms: int = Field(default=300, ge=0)
    summary_checkpoint_chars: int = Field(default=4000, ge=100)
    max_agent_turns: int = Field(default=20, ge=1)
    explorer_soft_turns: int = Field(default=3, ge=2)
    max_explorer_turns: int = Field(default=6, ge=3)
    max_explorer_tool_chars: int = Field(default=24_000, ge=1_000)
    max_committed_tool_chars: int = Field(default=16_000, ge=1_000)
    artifact_refresh_timeout_seconds: float = Field(default=30.0, ge=1.0, le=300.0)
    artifact_refresh_retries: int = Field(default=1, ge=0, le=3)
    artifact_refresh_retry_delay_seconds: float = Field(default=3.0, ge=0.0, le=60.0)
    exploration_goal_similarity: float = Field(default=0.72, ge=0.0, le=1.0)
    artifact_limit: int = Field(default=4, ge=0, le=20)
    log_enabled: bool = True
    log_level: str = "INFO"
    log_max_bytes: int = Field(default=10_000_000, ge=10_000)
    log_backup_count: int = Field(default=5, ge=1, le=100)

    def model_post_init(self, __context: object) -> None:
        self.workspace = self.workspace.expanduser().resolve()
        if self.state_dir is None:
            self.state_dir = self.workspace / ".hgi"
        else:
            self.state_dir = self.state_dir.expanduser().resolve()
        if self.explorer_soft_turns >= self.max_explorer_turns:
            raise ValueError("explorer_soft_turns must be lower than max_explorer_turns")
        self.log_level = self.log_level.upper()
        if self.log_level not in {"DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"}:
            raise ValueError("log_level must be DEBUG, INFO, WARNING, ERROR, or CRITICAL")

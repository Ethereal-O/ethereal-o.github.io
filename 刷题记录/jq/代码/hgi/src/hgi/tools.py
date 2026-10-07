from __future__ import annotations

import asyncio
import fnmatch
import inspect
import json
import os
import re
import shlex
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Awaitable, Callable, Protocol

from .config import WritePolicy

Handler = Callable[..., Any]
PermissionCallback = Callable[[str, Path], Awaitable[bool]]
MutationCallback = Callable[[set[str] | None], Any]

_EXCLUDED_DIRECTORY_NAMES = {
    ".git",
    ".hgi",
    ".mypy_cache",
    ".pytest_cache",
    ".ruff_cache",
    ".venv",
    "__pycache__",
    "node_modules",
    "venv",
}
_PUBLIC_ENV_SUFFIXES = (".example", ".sample", ".template")


class EventLogger(Protocol):
    def log(self, event: str, *, level: str = "INFO", **fields: Any) -> None: ...

    def increment(self, metric: str, amount: int = 1) -> None: ...


@dataclass(slots=True)
class Tool:
    name: str
    description: str
    input_schema: dict[str, Any]
    handler: Handler

    def schema(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "description": self.description,
            "input_schema": self.input_schema,
        }


class Dispatcher:
    def __init__(self, tools: list[Tool], *, logger: EventLogger | None = None):
        self._tools = {tool.name: tool for tool in tools}
        self.logger = logger

    @property
    def schemas(self) -> list[dict[str, Any]]:
        return [tool.schema() for tool in self._tools.values()]

    async def dispatch(self, name: str, arguments: dict[str, Any]) -> str:
        tool = self._tools.get(name)
        if tool is None:
            if self.logger:
                self.logger.log("tool.call.rejected", level="WARNING", tool=name, reason="unavailable")
            return f"Error: tool '{name}' is not available"
        started = time.perf_counter()
        if self.logger:
            self.logger.log(
                "tool.call.started",
                tool=name,
                arguments=self._safe_arguments(name, arguments),
            )
            self.logger.increment(f"tool_calls.{name}.started")
        try:
            if inspect.iscoroutinefunction(tool.handler):
                result = await tool.handler(**arguments)
            else:
                result = await asyncio.to_thread(tool.handler, **arguments)
            output = result if isinstance(result, str) else json.dumps(result, ensure_ascii=False)
            if self.logger:
                self.logger.log(
                    "tool.call.completed",
                    tool=name,
                    duration_ms=round((time.perf_counter() - started) * 1000, 3),
                    output_chars=len(output),
                    success=not output.startswith("Error:"),
                )
                self.logger.increment(f"tool_calls.{name}.completed")
            return output
        except asyncio.CancelledError:
            if self.logger:
                self.logger.log(
                    "tool.call.cancelled",
                    level="WARNING",
                    tool=name,
                    duration_ms=round((time.perf_counter() - started) * 1000, 3),
                )
                self.logger.increment(f"tool_calls.{name}.cancelled")
            raise
        except Exception as exc:
            if self.logger:
                self.logger.log(
                    "tool.call.failed",
                    level="ERROR",
                    tool=name,
                    duration_ms=round((time.perf_counter() - started) * 1000, 3),
                    error_type=type(exc).__name__,
                    error=str(exc),
                )
                self.logger.increment(f"tool_calls.{name}.failed")
            return f"Error: {type(exc).__name__}: {exc}"

    @staticmethod
    def _safe_arguments(name: str, arguments: dict[str, Any]) -> dict[str, Any]:
        safe: dict[str, Any] = {"argument_keys": sorted(arguments)}
        for key in ("path", "cwd", "start_line", "end_line", "timeout_seconds", "artifact_id"):
            if key in arguments:
                safe[key] = arguments[key]
        if "content" in arguments:
            safe["content_chars"] = len(str(arguments["content"]))
        if "pattern" in arguments:
            safe["pattern_chars"] = len(str(arguments["pattern"]))
        if "glob" in arguments:
            safe["glob"] = arguments["glob"]
        if "argv" in arguments:
            argv = arguments["argv"]
            safe["executable"] = argv[0] if isinstance(argv, list) and argv else ""
            safe["argc"] = len(argv) if isinstance(argv, list) else 0
        return safe


class WorkspaceTools:
    def __init__(
        self,
        root: Path,
        *,
        write_policy: WritePolicy = WritePolicy.DENY,
        permission: PermissionCallback | None = None,
        on_mutation: MutationCallback | None = None,
        logger: EventLogger | None = None,
    ):
        self.root = root.resolve()
        self.write_policy = write_policy
        self.permission = permission
        self.on_mutation = on_mutation
        self.logger = logger

    async def _notify_mutation(self, paths: set[str] | None) -> None:
        if self.on_mutation is None:
            return
        result = self.on_mutation(paths)
        if inspect.isawaitable(result):
            await result

    def _path(self, relative: str = ".") -> Path:
        candidate = (self.root / relative).resolve()
        if candidate != self.root and self.root not in candidate.parents:
            raise ValueError("path escapes the workspace")
        return candidate

    @staticmethod
    def _is_excluded(relative: Path) -> bool:
        if any(
            part in _EXCLUDED_DIRECTORY_NAMES or part.endswith(".egg-info")
            for part in relative.parts
        ):
            return True
        name = relative.name.lower()
        if name.endswith((".pyc", ".pyo")):
            return True
        return name == ".env" or (
            name.startswith(".env.") and not name.endswith(_PUBLIC_ENV_SUFFIXES)
        )

    def _read_path(self, relative: str = ".") -> Path:
        target = self._path(relative)
        if target != self.root and self._is_excluded(target.relative_to(self.root)):
            raise PermissionError("path is excluded from model access")
        return target

    def read_file(self, path: str, start_line: int = 1, end_line: int = 400) -> str:
        target = self._read_path(path)
        if not target.is_file():
            raise FileNotFoundError(path)
        if start_line < 1 or end_line < start_line:
            raise ValueError("invalid line range")
        lines = target.read_text(encoding="utf-8", errors="replace").splitlines()
        selected = lines[start_line - 1 : min(end_line, start_line + 999)]
        return "\n".join(f"{number}: {line}" for number, line in enumerate(selected, start_line))

    def glob_files(self, pattern: str = "**/*") -> str:
        matches: list[str] = []
        for path in self.root.glob(pattern):
            relative = path.relative_to(self.root)
            try:
                readable = self._read_path(relative.as_posix())
            except (PermissionError, ValueError):
                continue
            if readable.is_file():
                matches.append(relative.as_posix())
            if len(matches) >= 500:
                break
        return "\n".join(sorted(matches))

    def grep(self, pattern: str, glob: str = "**/*") -> str:
        regex = re.compile(pattern)
        matches: list[str] = []
        for current, directories, filenames in os.walk(self.root):
            current_path = Path(current)
            current_relative = current_path.relative_to(self.root)
            directories[:] = [
                directory
                for directory in directories
                if not self._is_excluded(current_relative / directory / ".keep")
            ]
            for filename in filenames:
                path = current_path / filename
                relative_path = path.relative_to(self.root)
                try:
                    path = self._read_path(relative_path.as_posix())
                except (PermissionError, ValueError):
                    continue
                relative = relative_path.as_posix()
                if not fnmatch.fnmatch(relative, glob) and not (
                    glob.startswith("**/") and fnmatch.fnmatch(relative, glob[3:])
                ):
                    continue
                try:
                    if path.stat().st_size > 2_000_000:
                        continue
                    for number, line in enumerate(
                        path.read_text(encoding="utf-8").splitlines(), 1
                    ):
                        if regex.search(line):
                            matches.append(f"{relative}:{number}:{line[:500]}")
                            if len(matches) >= 300:
                                return "\n".join(matches)
                except (UnicodeDecodeError, OSError):
                    continue
        return "\n".join(matches)

    async def git_diff(self, path: str = ".") -> str:
        target = self._read_path(path)
        relative = target.relative_to(self.root).as_posix()
        process = await asyncio.create_subprocess_exec(
            "git",
            "diff",
            "--",
            relative,
            cwd=self.root,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, stderr = await process.communicate()
        if process.returncode:
            raise RuntimeError(stderr.decode(errors="replace"))
        return stdout.decode(errors="replace")[:100_000]

    async def write_file(self, path: str, content: str) -> str:
        target = self._path(path)
        if self.write_policy == WritePolicy.DENY:
            if self.logger:
                self.logger.log("permission.denied", level="WARNING", operation="write_file", path=path)
            raise PermissionError("writes are disabled")
        if self.write_policy == WritePolicy.ASK:
            if self.permission is None or not await self.permission("write_file", target):
                if self.logger:
                    self.logger.log(
                        "permission.denied", level="WARNING", operation="write_file", path=path
                    )
                raise PermissionError("write was not approved")
            if self.logger:
                self.logger.log("permission.approved", operation="write_file", path=path)
        encoded = content.encode("utf-8")
        try:
            unchanged = target.is_file() and target.read_bytes() == encoded
        except OSError:
            unchanged = False
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(encoded)
        if not unchanged:
            await self._notify_mutation({target.relative_to(self.root).as_posix()})
        return f"Wrote {len(encoded)} bytes to {path}"

    def _workspace_snapshot(self) -> dict[str, tuple[int, int]]:
        snapshot: dict[str, tuple[int, int]] = {}
        for current, directories, filenames in os.walk(self.root):
            current_path = Path(current)
            current_relative = current_path.relative_to(self.root)
            directories[:] = [
                directory
                for directory in directories
                if not self._is_excluded(current_relative / directory / ".keep")
            ]
            for filename in filenames:
                relative = (current_relative / filename).as_posix()
                try:
                    path = self._read_path(relative)
                    stat = path.stat()
                except (OSError, PermissionError, ValueError):
                    continue
                if stat.st_size <= 2_000_000:
                    snapshot[relative] = (stat.st_mtime_ns, stat.st_size)
        return snapshot

    async def run_command(
        self, argv: list[str], cwd: str = ".", timeout_seconds: int = 120
    ) -> str:
        if not argv or not all(isinstance(item, str) and item for item in argv):
            raise ValueError("argv must contain at least one non-empty string")
        if timeout_seconds < 1 or timeout_seconds > 600:
            raise ValueError("timeout_seconds must be between 1 and 600")
        working_dir = self._path(cwd)
        if not working_dir.is_dir():
            raise NotADirectoryError(cwd)
        rendered = shlex.join(argv)
        if self.write_policy == WritePolicy.DENY:
            if self.logger:
                self.logger.log(
                    "permission.denied", level="WARNING", operation="run_command"
                )
            raise PermissionError("commands are disabled")
        if self.write_policy == WritePolicy.ASK:
            if self.permission is None or not await self.permission(
                f"run_command: {rendered}", working_dir
            ):
                if self.logger:
                    self.logger.log(
                        "permission.denied", level="WARNING", operation="run_command"
                    )
                raise PermissionError("command was not approved")
            if self.logger:
                self.logger.log("permission.approved", operation="run_command")
        before = await asyncio.to_thread(self._workspace_snapshot)
        process = await asyncio.create_subprocess_exec(
            *argv,
            cwd=working_dir,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
        )
        try:
            stdout, _ = await asyncio.wait_for(process.communicate(), timeout=timeout_seconds)
        except TimeoutError:
            process.kill()
            await process.wait()
            raise TimeoutError(f"command timed out after {timeout_seconds}s")
        finally:
            after = await asyncio.to_thread(self._workspace_snapshot)
            changed = {
                path
                for path in before.keys() | after.keys()
                if before.get(path) != after.get(path)
            }
            if changed:
                await self._notify_mutation(changed)
        output = stdout.decode(errors="replace")[:100_000]
        return f"exit_code={process.returncode}\n{output}"

    def readonly(self, extra: list[Tool] | None = None) -> Dispatcher:
        return Dispatcher(self._read_tools() + (extra or []), logger=self.logger)

    def committed(self) -> Dispatcher:
        return Dispatcher(
            self._read_tools()
            + [
                Tool(
                    "write_file",
                    "Write complete UTF-8 file contents. This operation is permission controlled.",
                    {
                        "type": "object",
                        "properties": {
                            "path": {"type": "string"},
                            "content": {"type": "string"},
                        },
                        "required": ["path", "content"],
                    },
                    self.write_file,
                ),
                Tool(
                    "run_command",
                    "Run an argv-style command in the workspace without shell expansion. "
                    f"Permission controlled. HGI's Python executable is {sys.executable!r}; "
                    "use it when the workspace does not define a different environment.",
                    {
                        "type": "object",
                        "properties": {
                            "argv": {
                                "type": "array",
                                "items": {"type": "string"},
                                "minItems": 1,
                            },
                            "cwd": {"type": "string"},
                            "timeout_seconds": {
                                "type": "integer",
                                "minimum": 1,
                                "maximum": 600,
                            },
                        },
                        "required": ["argv"],
                    },
                    self.run_command,
                ),
            ],
            logger=self.logger,
        )

    def _read_tools(self) -> list[Tool]:
        return [
            Tool(
                "read_file",
                "Read a UTF-8 text file inside the workspace with line numbers.",
                {
                    "type": "object",
                    "properties": {
                        "path": {"type": "string"},
                        "start_line": {"type": "integer", "minimum": 1},
                        "end_line": {"type": "integer", "minimum": 1},
                    },
                    "required": ["path"],
                },
                self.read_file,
            ),
            Tool(
                "glob_files",
                "List files matching a glob inside the workspace.",
                {
                    "type": "object",
                    "properties": {"pattern": {"type": "string"}},
                },
                self.glob_files,
            ),
            Tool(
                "grep",
                "Search text files using a regular expression.",
                {
                    "type": "object",
                    "properties": {
                        "pattern": {"type": "string"},
                        "glob": {"type": "string"},
                    },
                    "required": ["pattern"],
                },
                self.grep,
            ),
            Tool(
                "git_diff",
                "Read the current git diff for a workspace path.",
                {
                    "type": "object",
                    "properties": {"path": {"type": "string"}},
                },
                self.git_diff,
            ),
        ]

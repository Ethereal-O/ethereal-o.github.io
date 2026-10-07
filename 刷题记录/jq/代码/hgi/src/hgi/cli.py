from __future__ import annotations

import argparse
import asyncio
import os
from pathlib import Path

from dotenv import load_dotenv
from prompt_toolkit import PromptSession
from prompt_toolkit.application import get_app
from prompt_toolkit.key_binding import KeyBindings
from prompt_toolkit.patch_stdout import patch_stdout

from .config import ControlMode, HGIConfig, WritePolicy
from .controller import Controller
from .model import AnthropicBackend, OpenAIResponsesBackend
from .schemas import RunStatus, RuntimeEvent

AUTO_SENTINEL = "\0hgi-auto-commit\0"


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Speculative coding agent")
    result.add_argument("--workspace", type=Path, default=Path.cwd())
    result.add_argument(
        "--provider", choices=("codex", "anthropic"), default=os.getenv("HGI_PROVIDER", "codex")
    )
    result.add_argument("--model", default=os.getenv("HGI_MODEL", "gpt-5.6-luna"))
    result.add_argument(
        "--reasoning-effort", default=os.getenv("HGI_REASONING_EFFORT", "low")
    )
    result.add_argument(
        "--codex-home",
        type=Path,
        default=Path(os.getenv("HGI_CODEX_HOME", Path.home() / ".codex")),
    )
    control_modes = tuple(mode.value for mode in ControlMode)
    write_policies = tuple(policy.value for policy in WritePolicy)
    result.add_argument("--commit-mode", choices=control_modes, default=ControlMode.MANUAL)
    result.add_argument("--interrupt-mode", choices=control_modes, default=ControlMode.MANUAL)
    result.add_argument("--writes", choices=write_policies, default=WritePolicy.ASK)
    result.add_argument("--no-auto-explore", action="store_true")
    result.add_argument(
        "--log-level",
        choices=("DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"),
        default=os.getenv("HGI_LOG_LEVEL", "INFO").upper(),
    )
    result.add_argument(
        "--log-max-bytes", type=int, default=int(os.getenv("HGI_LOG_MAX_BYTES", "10000000"))
    )
    result.add_argument(
        "--log-backups", type=int, default=int(os.getenv("HGI_LOG_BACKUPS", "5"))
    )
    result.add_argument("--no-file-log", action="store_true")
    return result


async def run(args: argparse.Namespace) -> None:
    load_dotenv()
    status = {"text": "ready"}

    async def permission(operation: str, path: Path) -> bool:
        answer = await asyncio.to_thread(input, f"Allow {operation} {path}? [y/N] ")
        return answer.strip().lower() in {"y", "yes"}

    def on_event(event: RuntimeEvent) -> None:
        if event.kind == "explorer":
            status["text"] = f"explorer {event.status}: {event.message[:70]}"
        elif event.kind == "recommend_commit":
            status["text"] = "request appears complete; press Enter to send"
        elif event.kind == "committed":
            status["text"] = f"agent {event.status}"
            app = get_app()
            if event.status == RunStatus.RUNNING and app.is_running:
                app.exit(result=AUTO_SENTINEL)
        elif event.kind == "error":
            status["text"] = event.message[:100]
        app = get_app()
        if app.is_running:
            app.invalidate()

    config = HGIConfig(
        workspace=args.workspace,
        auto_explore=not args.no_auto_explore,
        commit_mode=args.commit_mode,
        interrupt_mode=args.interrupt_mode,
        writes=args.writes,
        log_enabled=not args.no_file_log,
        log_level=args.log_level,
        log_max_bytes=args.log_max_bytes,
        log_backup_count=args.log_backups,
    )
    if args.provider == "codex":
        model = OpenAIResponsesBackend.from_codex_config(
            model=args.model,
            reasoning_effort=args.reasoning_effort,
            codex_home=args.codex_home,
        )
    else:
        model = AnthropicBackend(args.model)
    controller = Controller(
        config,
        model,
        on_event=on_event,
        permission=permission,
    )
    bindings = KeyBindings()

    @bindings.add("escape")
    def _stop(event: object) -> None:
        asyncio.create_task(controller.stop())
        status["text"] = "stopped"

    session: PromptSession[str] = PromptSession(
        key_bindings=bindings,
        bottom_toolbar=lambda: f" HGI | {status['text']} | Esc stop, Ctrl-C exit ",
    )
    print(f"HGI workspace: {config.workspace}")
    try:
        while True:
            def attach_listener() -> None:
                get_app().current_buffer.on_text_changed += (
                    lambda buffer: controller.update_draft(buffer.text)
                )

            with patch_stdout():
                request = await session.prompt_async("hgi> ", pre_run=attach_listener)
            if request == AUTO_SENTINEL:
                try:
                    result = await controller.wait_for_commit()
                except asyncio.CancelledError:
                    continue
                if result:
                    print(f"\n{result}\n")
                continue
            if not request.strip():
                continue
            try:
                result = await controller.commit(request)
            except asyncio.CancelledError:
                continue
            except Exception as exc:
                print(f"Error: {exc}")
                continue
            print(f"\n{result}\n")
    except (EOFError, KeyboardInterrupt):
        pass
    finally:
        await controller.close()


def main() -> None:
    load_dotenv()
    asyncio.run(run(parser().parse_args()))


if __name__ == "__main__":
    main()

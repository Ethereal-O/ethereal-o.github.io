import sys

import pytest

from hgi.config import WritePolicy
from hgi.tools import WorkspaceTools


@pytest.mark.asyncio
async def test_explorer_dispatcher_has_no_write_tool(tmp_path):
    dispatcher = WorkspaceTools(tmp_path).readonly()
    assert {item["name"] for item in dispatcher.schemas} == {
        "read_file",
        "glob_files",
        "grep",
        "git_diff",
    }
    assert "not available" in await dispatcher.dispatch(
        "write_file", {"path": "oops", "content": "bad"}
    )


@pytest.mark.asyncio
async def test_paths_cannot_escape_workspace(tmp_path):
    tools = WorkspaceTools(tmp_path)
    result = await tools.readonly().dispatch("read_file", {"path": "../outside"})
    assert "escapes the workspace" in result


@pytest.mark.asyncio
async def test_read_tools_exclude_secrets_environments_and_generated_trees(tmp_path):
    (tmp_path / ".env").write_text("SECRET=env\n")
    (tmp_path / ".env.local").write_text("SECRET=local\n")
    (tmp_path / ".env.example").write_text("PUBLIC=value\n")
    (tmp_path / ".venv").mkdir()
    (tmp_path / ".venv" / "secret.py").write_text("SECRET = 'venv'\n")
    (tmp_path / "node_modules").mkdir()
    (tmp_path / "node_modules" / "secret.js").write_text("SECRET = 'node'\n")
    (tmp_path / "src").mkdir()
    (tmp_path / "src" / "app.py").write_text("VISIBLE = True\n")
    outside = tmp_path.parent / f"{tmp_path.name}-outside-secret"
    outside.write_text("SECRET = 'outside'\n")
    (tmp_path / "src" / "linked.py").symlink_to(outside)
    dispatcher = WorkspaceTools(tmp_path).readonly()

    inventory = await dispatcher.dispatch("glob_files", {"pattern": "**/*"})
    search = await dispatcher.dispatch("grep", {"pattern": "SECRET|VISIBLE"})
    blocked = await dispatcher.dispatch("read_file", {"path": ".env"})
    public_example = await dispatcher.dispatch("read_file", {"path": ".env.example"})

    assert "src/app.py" in inventory
    assert ".env.example" in inventory
    assert ".venv" not in inventory
    assert "node_modules" not in inventory
    assert "linked.py" not in inventory
    assert "src/app.py" in search
    assert "SECRET" not in search
    assert "excluded from model access" in blocked
    assert "PUBLIC=value" in public_example


@pytest.mark.asyncio
async def test_committed_write_obeys_permission(tmp_path):
    async def deny(_operation, _path):
        return False

    tools = WorkspaceTools(tmp_path, write_policy=WritePolicy.ASK, permission=deny)
    result = await tools.committed().dispatch(
        "write_file", {"path": "file.txt", "content": "data"}
    )
    assert "not approved" in result
    assert not (tmp_path / "file.txt").exists()


@pytest.mark.asyncio
async def test_committed_command_is_argv_based_and_permission_controlled(tmp_path):
    tools = WorkspaceTools(tmp_path, write_policy=WritePolicy.ALLOW)
    result = await tools.committed().dispatch(
        "run_command", {"argv": ["printf", "%s", "hello; not a shell"]}
    )

    assert "exit_code=0" in result
    assert "hello; not a shell" in result


@pytest.mark.asyncio
async def test_write_file_reports_only_real_mutations(tmp_path):
    mutations = []

    async def record(paths):
        mutations.append(paths)

    tools = WorkspaceTools(
        tmp_path, write_policy=WritePolicy.ALLOW, on_mutation=record
    )
    await tools.write_file("src/app.py", "VALUE = 1\n")
    await tools.write_file("src/app.py", "VALUE = 1\n")

    assert mutations == [{"src/app.py"}]


@pytest.mark.asyncio
async def test_run_command_reports_changed_new_and_deleted_files(tmp_path):
    (tmp_path / "changed.txt").write_text("before\n")
    (tmp_path / "deleted.txt").write_text("remove me\n")
    mutations = []

    async def record(paths):
        mutations.append(paths)

    tools = WorkspaceTools(
        tmp_path, write_policy=WritePolicy.ALLOW, on_mutation=record
    )
    result = await tools.run_command(
        [
            sys.executable,
            "-c",
            (
                "from pathlib import Path; "
                "Path('changed.txt').write_text('after\\n'); "
                "Path('created.txt').write_text('new\\n'); "
                "Path('deleted.txt').unlink()"
            ),
        ]
    )

    assert "exit_code=0" in result
    assert mutations == [{"changed.txt", "created.txt", "deleted.txt"}]


@pytest.mark.asyncio
async def test_run_command_without_changes_does_not_report_mutation(tmp_path):
    mutations = []
    tools = WorkspaceTools(
        tmp_path,
        write_policy=WritePolicy.ALLOW,
        on_mutation=lambda paths: mutations.append(paths),
    )

    await tools.run_command([sys.executable, "-c", "print('read only')"])

    assert mutations == []


def test_run_command_schema_exposes_active_python(tmp_path):
    dispatcher = WorkspaceTools(
        tmp_path, write_policy=WritePolicy.ALLOW
    ).committed()
    command = next(item for item in dispatcher.schemas if item["name"] == "run_command")

    assert sys.executable in command["description"]

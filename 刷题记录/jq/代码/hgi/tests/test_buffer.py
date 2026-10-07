from datetime import datetime, timedelta, timezone

from hgi.buffer import SpeculativeBuffer
from hgi.schemas import Artifact, Evidence


def test_buffer_round_trip_and_search(tmp_path):
    buffer = SpeculativeBuffer(tmp_path / ".hgi")
    artifact = Artifact(
        id="A1",
        generation=1,
        goal="inspect refresh token handling",
        summary="rotation happens in auth/session.py",
        findings=[Evidence(path="auth/session.py", line=42, claim="rotates token")],
        confidence=0.9,
        created_at=datetime.now(timezone.utc),
    )
    buffer.put(artifact)

    assert buffer.get("A1") == artifact
    assert buffer.search("refresh token", limit=1)[0].id == "A1"


def test_invalid_artifact_id_is_rejected(tmp_path):
    buffer = SpeculativeBuffer(tmp_path / ".hgi")
    try:
        buffer.get("../secret")
    except ValueError as exc:
        assert "invalid" in str(exc)
    else:
        raise AssertionError("path traversal was accepted")


def test_search_does_not_return_unrelated_fresh_artifact(tmp_path):
    buffer = SpeculativeBuffer(tmp_path / ".hgi")
    buffer.put(
        Artifact(
            id="A2",
            generation=1,
            goal="inspect payment retries",
            summary="billing worker",
            confidence=1.0,
        )
    )

    assert buffer.search("authentication session") == []


def test_workspace_buffer_persists_verified_evidence_and_invalidates_changes(tmp_path):
    source = tmp_path / "auth.py"
    source.write_text("TOKEN = 'first'\n")
    buffer = SpeculativeBuffer(tmp_path / ".hgi", workspace=tmp_path)
    artifact = Artifact(
        id="A3",
        generation=1,
        goal="inspect authentication token",
        summary="token evidence",
        findings=[
            Evidence(
                path="auth.py",
                line=1,
                claim="token is defined",
                excerpt="1: TOKEN = 'first'",
            )
        ],
        confidence=0.9,
    )

    assert buffer.put(artifact)
    cached = buffer.search("authentication token")[0]
    assert cached.findings[0].content_hash

    source.write_text("TOKEN = 'changed'\n")

    assert buffer.search("authentication token") == []
    stale = buffer.search("authentication token", include_stale=True)[0]
    assert stale.findings == []
    assert "Stale evidence must be refreshed: auth.py" in stale.unresolved


def test_workspace_buffer_rejects_low_quality_partial_artifact(tmp_path):
    (tmp_path / "auth.py").write_text("TOKEN = True\n")
    buffer = SpeculativeBuffer(tmp_path / ".hgi", workspace=tmp_path)
    partial = Artifact(
        id="A4",
        generation=1,
        goal="inspect auth",
        summary="partial inventory",
        findings=[Evidence(path="auth.py", claim="file exists")],
        confidence=0.3,
    )

    assert not buffer.put(partial)
    assert buffer.indexes() == []


def test_new_high_quality_artifact_supersedes_same_topic_cache(tmp_path):
    (tmp_path / "auth.py").write_text("TOKEN = True\n")
    buffer = SpeculativeBuffer(tmp_path / ".hgi", workspace=tmp_path)
    first = Artifact(
        id="Aold",
        generation=1,
        goal="inspect auth token",
        summary="old evidence",
        findings=[
            Evidence(path="auth.py", line=1, claim="old", excerpt="1: TOKEN = True")
        ],
        confidence=0.8,
    )
    replacement = Artifact(
        id="Anew",
        generation=2,
        goal="inspect authentication token",
        summary="new evidence",
        findings=[
            Evidence(path="auth.py", line=1, claim="new", excerpt="1: TOKEN = True"),
            Evidence(path="auth.py", claim="inventory only"),
        ],
        confidence=0.9,
    )

    assert buffer.put(first)
    assert buffer.put(replacement, supersedes=[first.id])

    assert [index.id for index in buffer.indexes()] == [replacement.id]
    assert not (buffer.artifacts_dir / f"{first.id}.json").exists()
    assert [finding.claim for finding in buffer.get(replacement.id).findings] == ["new"]


def test_eager_invalidation_removes_only_changed_evidence(tmp_path):
    (tmp_path / "auth.py").write_text("TOKEN = True\n")
    (tmp_path / "config.py").write_text("TIMEOUT = 30\n")
    buffer = SpeculativeBuffer(tmp_path / ".hgi", workspace=tmp_path)
    artifact = Artifact(
        id="Amixed",
        generation=1,
        goal="inspect authentication configuration",
        summary="token and timeout are configured",
        findings=[
            Evidence(
                path="auth.py",
                line=1,
                claim="token enabled",
                excerpt="1: TOKEN = True",
            ),
            Evidence(
                path="config.py",
                line=1,
                claim="timeout configured",
                excerpt="1: TIMEOUT = 30",
            ),
        ],
        confidence=0.9,
    )
    assert buffer.put(artifact)

    assert buffer.invalidate({"auth.py"}) == 1

    stale = buffer.search("authentication configuration", include_stale=True)[0]
    assert [finding.path for finding in stale.findings] == ["config.py"]
    assert stale.summary.startswith("Cached summary invalidated")
    assert "Stale evidence must be refreshed: auth.py" in stale.unresolved
    assert stale.confidence == 0.5
    assert buffer.search("authentication configuration")[0].findings[0].path == "config.py"
    by_stale_path = buffer.search("auth.py", include_stale=True)[0]
    assert by_stale_path.id == artifact.id
    assert all(finding.path != "auth.py" for finding in by_stale_path.findings)


def test_search_selects_only_query_relevant_evidence(tmp_path):
    (tmp_path / "orders.py").write_text("MAX_RETRIES = 2\n")
    (tmp_path / "cache.py").write_text("TTL_SECONDS = 30\n")
    buffer = SpeculativeBuffer(tmp_path / ".hgi", workspace=tmp_path)
    assert buffer.put(
        Artifact(
            id="Abroad",
            generation=1,
            goal="inspect order retry behavior",
            summary="order retry analysis",
            findings=[
                Evidence(
                    path="orders.py",
                    line=1,
                    claim="maximum retries",
                    excerpt="1: MAX_RETRIES = 2",
                ),
                Evidence(
                    path="cache.py",
                    line=1,
                    claim="cache TTL boundary",
                    excerpt="1: TTL_SECONDS = 30",
                ),
            ],
            confidence=0.8,
        )
    )

    result = buffer.search("只读分析 cache TTL 边界，不要修改文件")

    assert len(result) == 1
    assert [finding.path for finding in result[0].findings] == ["cache.py"]
    assert len(buffer.get("Abroad").findings) == 2


def test_technical_query_does_not_match_only_generic_topic_words(tmp_path):
    (tmp_path / "orders.py").write_text("MAX_RETRIES = 2\n")
    buffer = SpeculativeBuffer(tmp_path / ".hgi", workspace=tmp_path)
    assert buffer.put(
        Artifact(
            id="Aorders",
            generation=1,
            goal="分析订单重试边界和测试语义",
            summary="订单边界",
            findings=[
                Evidence(
                    path="orders.py",
                    line=1,
                    claim="重试边界测试",
                    excerpt="1: MAX_RETRIES = 2",
                )
            ],
            confidence=0.8,
        )
    )

    assert buffer.search("分析 CacheEntry TTL 边界和测试语义") == []


def test_expired_provisional_artifact_is_not_reused(tmp_path):
    (tmp_path / "auth.py").write_text("TOKEN = True\n")
    buffer = SpeculativeBuffer(tmp_path / ".hgi", workspace=tmp_path)
    assert buffer.put(
        Artifact(
            id="Aprovisional",
            generation=1,
            goal="inspect authentication",
            summary="partial authentication evidence",
            findings=[
                Evidence(
                    path="auth.py",
                    line=1,
                    claim="token enabled",
                    excerpt="1: TOKEN = True",
                )
            ],
            confidence=0.5,
            provisional=True,
            created_at=datetime.now(timezone.utc) - timedelta(hours=2),
        )
    )

    assert buffer.search("authentication") == []
    assert buffer.indexes() == []
    assert not (buffer.artifacts_dir / "Aprovisional.json").exists()

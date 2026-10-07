from __future__ import annotations

import hashlib
import json
import re
from datetime import datetime, timedelta, timezone
from pathlib import Path

from .schemas import Artifact, ArtifactIndex


_SEARCH_STOP_TERMS = {
    "analyze",
    "analysis",
    "code",
    "current",
    "explain",
    "file",
    "files",
    "inspect",
    "modify",
    "readonly",
    "不要",
    "代码",
    "修改",
    "分析",
    "只读",
    "当前",
    "指出",
    "文件",
    "是否",
    "现有",
    "测试",
    "继续",
    "说明",
    "边界",
    "语义",
    "问题",
}


def _search_terms(value: str) -> set[str]:
    lowered = value.lower()
    words = {
        term
        for term in re.findall(r"[a-z0-9_./-]+", lowered)
        if len(term) > 1 and term not in _SEARCH_STOP_TERMS
    }
    terms = words | {term[:4] for term in words if len(term) >= 6}
    for run in re.findall(r"[\u3400-\u9fff]+", lowered):
        terms.update(
            token
            for index in range(len(run) - 1)
            if (token := run[index : index + 2]) not in _SEARCH_STOP_TERMS
        )
    return terms


def _search_anchors(value: str) -> set[str]:
    words = {
        term
        for term in re.findall(r"[a-z0-9_./-]+", value.lower())
        if len(term) > 1 and term not in _SEARCH_STOP_TERMS
    }
    return words | {term[:4] for term in words if len(term) >= 6}


class SpeculativeBuffer:
    def __init__(
        self,
        root: Path,
        *,
        workspace: Path | None = None,
        min_persist_confidence: float = 0.5,
    ):
        self.root = root
        self.workspace = workspace.resolve() if workspace else None
        self.min_persist_confidence = min_persist_confidence
        self.artifacts_dir = root / "artifacts"
        self.index_path = root / "index.jsonl"
        self.artifacts_dir.mkdir(parents=True, exist_ok=True)

    def put(self, artifact: Artifact, *, supersedes: list[str] | None = None) -> bool:
        stored = artifact.model_copy(deep=True)
        if self.workspace:
            verified = []
            for finding in stored.findings:
                content_hash = self._content_hash(finding.path)
                if content_hash is None:
                    continue
                finding.content_hash = content_hash
                if finding.excerpt or finding.line is not None:
                    verified.append(finding)
            stored.findings = verified
            if stored.confidence < self.min_persist_confidence or not verified:
                return False

        payload = stored.model_dump_json(indent=2)
        (self.artifacts_dir / f"{stored.id}.json").write_text(payload, encoding="utf-8")
        index = ArtifactIndex(
            id=stored.id,
            goal=stored.goal,
            summary=stored.summary,
            paths=sorted({finding.path for finding in stored.findings}),
            confidence=stored.confidence,
            provisional=stored.provisional,
            created_at=stored.created_at,
        )
        with self.index_path.open("a", encoding="utf-8") as stream:
            stream.write(index.model_dump_json() + "\n")
        self._remove_superseded(stored.id, supersedes or [])
        return True

    def _remove_superseded(self, replacement_id: str, supersedes: list[str]) -> None:
        removed = {artifact_id for artifact_id in supersedes if artifact_id != replacement_id}
        if not removed:
            return
        for artifact_id in removed:
            if re.fullmatch(r"[A-Za-z0-9_-]+", artifact_id):
                (self.artifacts_dir / f"{artifact_id}.json").unlink(missing_ok=True)
        retained = [index for index in self.indexes() if index.id not in removed]
        self.index_path.write_text(
            "".join(index.model_dump_json() + "\n" for index in retained),
            encoding="utf-8",
        )

    def get(self, artifact_id: str) -> Artifact:
        if not re.fullmatch(r"[A-Za-z0-9_-]+", artifact_id):
            raise ValueError("invalid artifact id")
        artifact = Artifact.model_validate_json(
            (self.artifacts_dir / f"{artifact_id}.json").read_text(encoding="utf-8")
        )
        return self._validated(artifact)

    def _validated(self, artifact: Artifact) -> Artifact:
        if self.workspace is None:
            return artifact
        refreshed = artifact.model_copy(deep=True)
        valid = []
        stale_paths = []
        for finding in refreshed.findings:
            current_hash = self._content_hash(finding.path)
            if finding.content_hash and current_hash == finding.content_hash:
                valid.append(finding)
            else:
                stale_paths.append(finding.path)
        refreshed.findings = valid
        for path in sorted(set(stale_paths)):
            message = f"Stale evidence must be refreshed: {path}"
            if message not in refreshed.unresolved:
                refreshed.unresolved.append(message)
        if stale_paths:
            retained = len(valid) / max(len(valid) + len(stale_paths), 1)
            refreshed.confidence = min(refreshed.confidence, retained)
            refreshed.summary = (
                "Cached summary invalidated by workspace changes; use only retained "
                "evidence and refresh stale paths."
            )
        return refreshed

    def invalidate(self, paths: set[str] | None = None) -> int:
        """Eagerly remove evidence affected by known workspace mutations."""
        normalized = {Path(path).as_posix() for path in paths} if paths is not None else None
        changed = 0
        updated_indexes: list[ArtifactIndex] = []
        for index in self.indexes():
            artifact_path = self.artifacts_dir / f"{index.id}.json"
            try:
                artifact = Artifact.model_validate_json(artifact_path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                continue
            stale = [
                finding
                for finding in artifact.findings
                if normalized is None or Path(finding.path).as_posix() in normalized
            ]
            if stale:
                stale_keys = {(finding.path, finding.line, finding.claim) for finding in stale}
                artifact.findings = [
                    finding
                    for finding in artifact.findings
                    if (finding.path, finding.line, finding.claim) not in stale_keys
                ]
                for path in sorted({finding.path for finding in stale}):
                    message = f"Stale evidence must be refreshed: {path}"
                    if message not in artifact.unresolved:
                        artifact.unresolved.append(message)
                retained = len(artifact.findings) / max(len(artifact.findings) + len(stale), 1)
                artifact.confidence = min(artifact.confidence, retained)
                artifact.summary = (
                    "Cached summary invalidated by workspace changes; use only retained "
                    "evidence and refresh stale paths."
                )
                self._atomic_write(artifact_path, artifact.model_dump_json(indent=2))
                changed += 1
            updated_indexes.append(
                ArtifactIndex(
                    id=artifact.id,
                    goal=artifact.goal,
                    summary=artifact.summary,
                    # Keep stale paths searchable without retaining their old evidence.
                    paths=sorted(
                        {finding.path for finding in artifact.findings}
                        | {finding.path for finding in stale}
                    ),
                    confidence=artifact.confidence,
                    provisional=artifact.provisional,
                    created_at=artifact.created_at,
                )
            )
        if changed:
            self._atomic_write(
                self.index_path,
                "".join(index.model_dump_json() + "\n" for index in updated_indexes),
            )
        return changed

    @staticmethod
    def _atomic_write(path: Path, content: str) -> None:
        temporary = path.with_suffix(path.suffix + ".tmp")
        temporary.write_text(content, encoding="utf-8")
        temporary.replace(path)

    def _content_hash(self, relative: str) -> str | None:
        if self.workspace is None or not relative:
            return None
        try:
            candidate = (self.workspace / relative).resolve()
            if candidate != self.workspace and self.workspace not in candidate.parents:
                return None
            relative_path = candidate.relative_to(self.workspace)
            excluded_parts = {
                ".git",
                ".hgi",
                ".venv",
                "__pycache__",
                "node_modules",
                "venv",
            }
            if any(part in excluded_parts for part in relative_path.parts):
                return None
            name = relative_path.name.lower()
            if name == ".env" or (
                name.startswith(".env.")
                and not name.endswith((".example", ".sample", ".template"))
            ):
                return None
            if not candidate.is_file() or candidate.stat().st_size > 2_000_000:
                return None
            digest = hashlib.sha256()
            with candidate.open("rb") as stream:
                for chunk in iter(lambda: stream.read(65_536), b""):
                    digest.update(chunk)
            return digest.hexdigest()
        except OSError:
            return None

    def indexes(self) -> list[ArtifactIndex]:
        if not self.index_path.exists():
            return []
        latest: dict[str, ArtifactIndex] = {}
        for line in self.index_path.read_text(encoding="utf-8").splitlines():
            try:
                index = ArtifactIndex.model_validate_json(line)
            except ValueError:
                continue
            latest[index.id] = index
        return list(latest.values())

    def search(
        self, query: str, limit: int = 4, *, include_stale: bool = False
    ) -> list[Artifact]:
        if limit <= 0:
            return []
        terms = _search_terms(query)
        anchors = _search_anchors(query)
        if not terms:
            return []
        now = datetime.now(timezone.utc)
        self._prune_expired_provisional(now)

        def overlap(index: ArtifactIndex) -> int:
            haystack = f"{index.goal} {index.summary} {' '.join(index.paths)}".lower()
            return sum(1 for term in terms if term in haystack)

        def score(index: ArtifactIndex) -> float:
            term_overlap = overlap(index)
            age_hours = max((now - index.created_at).total_seconds() / 3600, 0)
            freshness = 1 / (1 + age_hours / 24)
            return term_overlap * 3 + index.confidence + freshness

        candidates = [
            index
            for index in self.indexes()
            if overlap(index) > 0
            and (
                not anchors
                or any(
                    anchor
                    in f"{index.goal} {index.summary} {' '.join(index.paths)}".lower()
                    for anchor in anchors
                )
            )
        ]
        ranked = sorted(candidates, key=score, reverse=True)
        artifacts = []
        for index in ranked:
            artifact = self.get(index.id)
            if artifact.provisional and now - artifact.created_at > timedelta(hours=1):
                continue
            artifacts.append(
                self._select_evidence(artifact, anchors if anchors else terms)
            )
        reusable = [artifact for artifact in artifacts if artifact.findings or include_stale]
        return reusable[:limit]

    def _prune_expired_provisional(self, now: datetime) -> None:
        expired = [
            index.id
            for index in self.indexes()
            if index.provisional and now - index.created_at > timedelta(hours=1)
        ]
        if expired:
            self._remove_superseded("", expired)

    @staticmethod
    def _select_evidence(artifact: Artifact, terms: set[str]) -> Artifact:
        """Return a query-focused copy while preserving the durable Artifact."""
        selected = artifact.model_copy(deep=True)

        def matches(value: str) -> bool:
            lowered = value.lower()
            return any(term in lowered for term in terms)

        matching_paths = {
            finding.path
            for finding in selected.findings
            if matches(f"{finding.path} {finding.claim} {finding.excerpt}")
        }
        if not matching_paths:
            return selected
        selected.findings = [
            finding for finding in selected.findings if finding.path in matching_paths
        ]
        selected.unresolved = [
            item
            for item in selected.unresolved
            if matches(item) or any(path in item for path in matching_paths)
        ]
        return selected

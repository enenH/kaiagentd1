#!/usr/bin/env python3
"""Check a proposed public source tree without printing sensitive values.

By default the Git index is scanned, including staged bytes even if the working
copy was subsequently changed. Use --source worktree for tracked working files,
or --source directory to inspect an export before initializing Git. This is a
repository-specific regression guard, not a substitute for a security review.
"""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys


# Only the official Gradle 9.4.1 wrapper is admitted as a binary. When updating
# Gradle, verify the replacement against https://gradle.org/release-checksums/
# and update this exact path/hash entry in the same reviewed change.
REVIEWED_BINARIES = {
    "app/gradle/wrapper/gradle-wrapper.jar":
        "55243ef57851f12b070ad14f7f5bb8302daceeebc5bce5ece5fa6edb23e1145c",
}
BINARY_SUFFIXES = {
    ".so", ".tflite", ".apk", ".aab", ".aar", ".jar", ".dex", ".bin",
    ".dll", ".dylib", ".exe", ".a", ".o", ".zip", ".7z", ".tar",
    ".gz", ".xz", ".bz2", ".onnx", ".pt", ".pth", ".safetensors",
}
PRIVATE_SUFFIXES = {".jks", ".keystore", ".p12", ".pfx", ".key", ".pem"}
PRIVATE_NAMES = {
    "local.properties", "keystore.properties", "key.properties",
    "credentials.json", "secrets.json", "secrets.yml", "secrets.yaml",
    "id_rsa", "id_dsa", "id_ecdsa", "id_ed25519", "auth.json",
}
ENV_TEMPLATES = {".env.example", ".env.sample", ".env.template"}
# This reviewed map names environment variables; it does not hold their values.
CREDENTIAL_REFERENCE_ALLOWLIST = {
    ("app/release-signing.gradle", "KAIAGENTD_STORE_PASSWORD"),
    ("app/release-signing.gradle", "KAIAGENTD_KEY_PASSWORD"),
}
SOURCE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".java", ".kt",
    ".py", ".js", ".ts", ".tsx", ".jsx", ".gradle", ".cmake",
    ".sh", ".bat", ".ps1", ".yml", ".yaml", ".json", ".properties",
}

# The checker necessarily names the removed symbols in its rules. Only those
# symbol checks are exempt here; credential and binary checks still apply.
AUTH_RULE_EXEMPTIONS = {"scripts/check_public_tree.py"}
AUTH_SYMBOLS = re.compile(
    r"\bauth\s*::|\bnamespace\s+auth\b|"
    r"#\s*include\s*[<\"][^>\"]*\bauth(?:_[\w]+)?\.h[>\"]|"
    r"\b(?:StoredAuth|AUTH_CONFIG_PATH|KEY_CARD|cardEdit_|validateCard|"
    r"saveCard|cardValue|writeAuthJson|readAuthJson|s_guardArmed|"
    r"s_workerPulseMs|s_mainPulseMs)\b|卡密|注册码|机器码"
)
AUTH_FILENAME = re.compile(
    r"(?i)^auth(?:[_-][\w-]+)?\.(?:c|cc|cpp|cxx|h|hpp|java|kt|py|js|ts)$"
)
VALUE_RULES = {
    "private-key-material": re.compile(r"-----BEGIN (?:[A-Z]+ )?PRIVATE KEY-----"),
    "github-token": re.compile(
        r"\b(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,})\b"
    ),
    "cloud-access-key": re.compile(r"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b"),
    "service-token": re.compile(
        r"\b(?:sk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{20,}|"
        r"xox[baprs]-[A-Za-z0-9-]{15,})\b"
    ),
    "telegram-token": re.compile(r"\b\d{7,12}:[A-Za-z0-9_-]{30,}\b"),
    "jwt-token": re.compile(
        r"\beyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\b"
    ),
    "credential-in-url": re.compile(r"https?://[^/@\s]+:[^/@\s]+@"),
}
SECRET_ASSIGNMENT = re.compile(
    r"(?i)\b(?:[A-Za-z_][A-Za-z0-9_]*(?:password|passwd)|"
    r"password|passwd|api[_-]?key|app[_-]?key|secret(?:[_-]?key)?|"
    r"access[_-]?token|refresh[_-]?token|bot[_-]?token)\b[\"']?"
    r"\s*(?:[:=]\s*|\s+)(?:OBF\s*\(\s*)?"
    r"(?P<quote>[\"'])(?P<value>[^\"'\r\n]+)(?P=quote)"
)
UNQUOTED_SECRET_ASSIGNMENT = re.compile(
    r"(?i)^\s*(?:export\s+)?(?:[A-Za-z_][A-Za-z0-9_]*(?:password|passwd)|"
    r"password|passwd|api[_-]?key|app[_-]?key|secret(?:[_-]?key)?|"
    r"access[_-]?token|refresh[_-]?token|bot[_-]?token)\s*=\s*"
    r"(?P<value>[^\"'#\r\n][^#\r\n]*)$"
)
PLACEHOLDER = re.compile(
    r"(?i)^(?:<[^>]+>|\$\{[A-Za-z_][A-Za-z0-9_]*\}|%[A-Za-z_][A-Za-z0-9_]*%|"
    r"example|placeholder|replace[-_]me|your[-_][A-Za-z0-9_-]+)$"
)
PERSONAL_PATH = re.compile(
    r"(?i)(?:\b[A-Z]:[\\/](?:Users|Documents and Settings|WorkDir|Android)[\\/]|"
    r"/(?:home|Users)/[A-Za-z0-9_.-]+/)"
)
PRIVATE_NOTIFICATIONS = re.compile(
    r"\bTG_(?:BOT_TOKEN|CHAT_ID|API_ID|API_HASH)\b|telegram-bot-api|api\.telegram\.org/bot"
)
IPV4 = re.compile(r"(?<![\w.])(?:\d{1,3}\.){3}\d{1,3}(?![\w.])")
DOCUMENTATION_NETWORKS = tuple(ipaddress.ip_network(n) for n in (
    "192.0.2.0/24", "198.51.100.0/24", "203.0.113.0/24",
))


def inspect_blob(path: str, data: bytes, mode: str = "100644") -> list[tuple[str, int, str]]:
    """Return only (path, line, category); matched values are never returned."""
    findings: list[tuple[str, int, str]] = []

    def flag(line: int, category: str) -> None:
        item = (path, line, category)
        if item not in findings:
            findings.append(item)

    p = PurePosixPath(path)
    name = p.name.lower()
    suffix = p.suffix.lower()
    if mode not in {"100644", "100755"}:
        flag(1, "unreviewed-symlink-or-submodule")
        return findings
    if (suffix in PRIVATE_SUFFIXES or name in PRIVATE_NAMES or
            (name.startswith(".env") and name not in ENV_TEMPLATES) or
            any(part in {".aws", ".ssh", ".idea", ".gradle"} for part in p.parts)):
        flag(1, "private-or-local-file")
    if AUTH_FILENAME.fullmatch(name):
        flag(1, "removed-auth-file")
    if path in REVIEWED_BINARIES:
        if hashlib.sha256(data).hexdigest() != REVIEWED_BINARIES[path]:
            flag(1, "unverified-wrapper-binary")
        return findings
    if suffix in BINARY_SUFFIXES:
        flag(1, "unreviewed-runtime-or-build-artifact")
    if len(data) > 2_000_000:
        flag(1, "oversized-unreviewed-file")
        return findings
    try:
        content = data.decode("utf-8")
    except UnicodeDecodeError:
        flag(1, "unreviewed-binary")
        return findings
    if "\x00" in content:
        flag(1, "unreviewed-binary")
        return findings
    source_file = suffix in SOURCE_SUFFIXES or name == "cmakelists.txt"
    for line_number, line in enumerate(content.splitlines(), 1):
        for category, pattern in VALUE_RULES.items():
            if pattern.search(line):
                flag(line_number, category)
        for match in SECRET_ASSIGNMENT.finditer(line):
            value = match.group("value")
            if ((path, value) not in CREDENTIAL_REFERENCE_ALLOWLIST and
                    not PLACEHOLDER.fullmatch(value)):
                flag(line_number, "hardcoded-credential")
        if name in ENV_TEMPLATES or suffix == ".properties":
            match = UNQUOTED_SECRET_ASSIGNMENT.search(line)
            if match:
                value = match.group("value").strip()
                if value and not PLACEHOLDER.fullmatch(value):
                    flag(line_number, "hardcoded-credential")
        if source_file:
            if path not in AUTH_RULE_EXEMPTIONS and AUTH_SYMBOLS.search(line):
                flag(line_number, "removed-auth-flow")
            if PERSONAL_PATH.search(line):
                flag(line_number, "personal-build-path")
        if path.startswith(".github/") and PRIVATE_NOTIFICATIONS.search(line):
            flag(line_number, "private-notification-config")
        for match in IPV4.finditer(line):
            try:
                address = ipaddress.ip_address(match.group())
            except ValueError:
                continue
            if (address.is_private and not address.is_loopback and
                    not address.is_unspecified and
                    not any(address in network for network in DOCUMENTATION_NETWORKS)):
                flag(line_number, "private-network-address")
    return findings


def git(root: Path, *args: str) -> bytes:
    result = subprocess.run(
        ["git", "--no-optional-locks", "-C", str(root), *args],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True,
    )
    return result.stdout


def source_files(root: Path, source: str):
    if source == "directory":
        for file in sorted(root.rglob("*")):
            relative = file.relative_to(root)
            if ".git" in relative.parts:
                continue
            if file.is_symlink():
                yield relative.as_posix(), b"", "120000"
            elif file.is_file():
                yield relative.as_posix(), file.read_bytes(), "100644"
        return
    actual_root = Path(git(root, "rev-parse", "--show-toplevel").decode().strip()).resolve()
    if actual_root != root:
        raise ValueError("The selected directory is not the repository root.")
    for entry in git(root, "ls-files", "--stage", "-z").split(b"\x00"):
        if not entry:
            continue
        metadata, raw_path = entry.split(b"\t", 1)
        mode, sha, stage = metadata.decode("ascii").split()
        path = raw_path.decode("utf-8", errors="surrogateescape")
        if stage != "0":
            raise ValueError("Resolve index conflicts before scanning.")
        if mode not in {"100644", "100755"}:
            yield path, b"", mode
        elif source == "index":
            yield path, git(root, "cat-file", "blob", sha), mode
        else:
            file = root / path
            if file.is_symlink():
                yield path, b"", "120000"
            elif file.is_file():
                yield path, file.read_bytes(), mode
            else:
                raise ValueError("A tracked file is missing; stage its deletion first.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--source", choices=("index", "worktree", "directory"), default="index")
    args = parser.parse_args()
    findings = []
    count = 0
    try:
        for path, data, mode in source_files(args.root.resolve(), args.source):
            count += 1
            findings.extend(inspect_blob(path, data, mode))
    except (OSError, ValueError, subprocess.CalledProcessError):
        print("Public tree check could not read the selected source; check the root and index.", file=sys.stderr)
        return 2
    if not count:
        print("Public tree check refused an empty source.", file=sys.stderr)
        return 2
    for path, line, category in sorted(set(findings)):
        print(f"{json.dumps(path, ensure_ascii=True)}:{line}: {category}")
    if findings:
        print(f"Public tree check failed: {len(set(findings))} findings in {count} files.")
        return 1
    print(f"Public tree check passed: {count} files scanned from {args.source}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

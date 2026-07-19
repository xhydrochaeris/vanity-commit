#!/usr/bin/env python3
"""
vanity_commit.py — construct a git commit object, search for a vanity hash
prefix using the vanity_cpu backend, and finalize the winning commit.

Assumes vanity_cpu is already built and sitting next to this script (or on
PATH) — no backend selection / auto-build yet, that's a later version.
"""

import subprocess
import sys
import argparse
from pathlib import Path

BACKEND_BINARY = Path(__file__).parent / "vanity_cpu"


def run_git(args, input_text=None, check=True):
    """Run a git command, return stripped stdout. Raises on failure if check=True."""
    result = subprocess.run(
        ["git", *args],
        input=input_text,
        capture_output=True,
        text=True,
    )
    if check and result.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def build_commit_content(message: str) -> str:
    """
    Reconstruct the exact bytes git would use for a new commit object,
    minus the "commit <len>\\0" header (that part is added by whoever
    hashes/writes the object, not stored here).
    """
    tree = run_git(["write-tree"])

    # First commit in a repo has no parent — rev-parse HEAD fails, and that's expected.
    parent_line = ""
    parent_check = subprocess.run(
        ["git", "rev-parse", "--verify", "-q", "HEAD"],
        capture_output=True, text=True,
    )
    if parent_check.returncode == 0:
        parent = parent_check.stdout.strip()
        parent_line = f"parent {parent}\n"

    author_ident = run_git(["var", "GIT_AUTHOR_IDENT"])
    committer_ident = run_git(["var", "GIT_COMMITTER_IDENT"])

    content = (
        f"tree {tree}\n"
        f"{parent_line}"
        f"author {author_ident}\n"
        f"committer {committer_ident}\n"
        f"\n"
        f"{message}"
    )
    return content


def run_backend(content_before_trailer: str, target: str) -> dict:
    if not BACKEND_BINARY.exists():
        sys.exit(
            f"error: backend binary not found at {BACKEND_BINARY}\n"
            f"build it first: gcc -O3 -fopenmp vanity_cpu.c -o vanity_cpu -lcrypto"
        )

    result = subprocess.run(
        [str(BACKEND_BINARY), content_before_trailer, target],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        sys.exit(f"backend failed: {result.stderr.strip()}")

    print(result.stderr, end="", file=sys.stderr)  # throughput stats

    parsed = {}
    for line in result.stdout.strip().splitlines():
        key, _, value = line.partition(": ")
        parsed[key] = value
    return parsed


def finalize_commit(content_before_trailer: str, trailer: str, expected_hash: str):
    """Write the winning commit object for real and move the current branch to it."""
    full_content = content_before_trailer + trailer

    actual_hash = run_git(["hash-object", "-w", "-t", "commit", "--stdin"], input_text=full_content)

    if actual_hash != expected_hash:
        sys.exit(
            f"error: mismatch between predicted hash ({expected_hash}) and what git "
            f"actually wrote ({actual_hash}) — refusing to move the branch. "
            f"(Likely cause: working tree/index changed between search and finalize.)"
        )

    branch_check = subprocess.run(
        ["git", "symbolic-ref", "--short", "-q", "HEAD"],
        capture_output=True, text=True,
    )
    if branch_check.returncode != 0:
        sys.exit("error: HEAD is not on a branch (detached HEAD?) — not moving anything automatically.")

    branch = branch_check.stdout.strip()
    run_git(["update-ref", f"refs/heads/{branch}", actual_hash])
    print(f"done: {branch} now points to {actual_hash}")


def confirm(prompt: str) -> bool:
    """Ask a y/n question; only an explicit 'y' counts as yes."""
    answer = input(f"{prompt} [y/N] ").strip().lower()
    return answer == "y"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-m", "--message", required=True, help="commit message")
    parser.add_argument("target", help="desired lowercase hex prefix for the commit hash")
    args = parser.parse_args()

    content = build_commit_content(args.message)
    result = run_backend(content, args.target)

    print(f"found: {result['hash']}")
    # Reconstruct the trailer from the counter rather than parsing the
    # backend's "trailer:" line directly — that field embeds its own
    # newline (the format is "\nVanity: N"), which breaks naive
    # line-by-line parsing of the backend's stdout.
    trailer = f"\n\nVanity: {result['counter']}"
    if not confirm(f"Commit as {result['hash']}?"):
        print("aborted — nothing was written.")
        return
    finalize_commit(content, trailer, result["hash"])


if __name__ == "__main__":
    main()

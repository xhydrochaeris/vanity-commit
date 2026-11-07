#!/usr/bin/env python3
"""
vanity_commit.py: CLI to construct a git commit object, search for a vanity hash
prefix using the vanity_cpu backend, and finalize the winning commit.
"""

import subprocess
import sys, os
import argparse
from pathlib import Path

BACKEND_BINARY = Path(__file__).parent / "vanity_cpu"
AUTO_PARENT = object()


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


def get_commit_message(initial_message="") -> str:
    editor = run_git(["var", "GIT_EDITOR"])

    import shlex
    import tempfile

    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".gitmessage", delete=False
    ) as f:
        message_file = Path(f.name)
        f.write(initial_message)

    try:
        subprocess.run(
            f"{editor} {shlex.quote(str(message_file))}",
            shell=True,
            check=True
        )

        return message_file.read_text()

    finally:
        message_file.unlink(missing_ok=True)


def get_commit_metadata(commit: str) -> dict:
    result = subprocess.run(
        [
            "git", "show", "-s",
            "--format=format:%B%x00%an%x00%ae%x00%aI",
            commit,
        ],
        capture_output=True,
        text=True,
        check=True,
    )

    message, name, email, date = result.stdout.split("\0", 3)

    return {
        "message": message,
        "author": (name, email),
        "date": date.rstrip("\n"),
    }


def build_commit_content(message: str, parent=AUTO_PARENT, author=None, date=None) -> str:
    tree = run_git(["write-tree"])

    if parent is AUTO_PARENT:
        parent_check = subprocess.run(
            ["git", "rev-parse", "--verify", "-q", "HEAD"],
            capture_output=True, text=True,
        )
        parent = parent_check.stdout.strip() if parent_check.returncode == 0 else None

    env = os.environ.copy()

    if author is not None:
        env["GIT_AUTHOR_NAME"] = author[0]
        env["GIT_AUTHOR_EMAIL"] = author[1]

    if date is not None:
        env["GIT_AUTHOR_DATE"] = date

    author_ident = subprocess.run(
        ["git", "var", "GIT_AUTHOR_IDENT"],
        capture_output=True, text=True, check=True, env=env,
    ).stdout.strip()

    committer_ident = run_git(["var", "GIT_COMMITTER_IDENT"])

    parent_line = f"parent {parent}\n" if parent else ""

    return (
        f"tree {tree}\n"
        f"{parent_line}"
        f"author {author_ident}\n"
        f"committer {committer_ident}\n"
        f"\n"
        f"{message}"
    )


def parse_author(author: str) -> tuple[str, str]:
    name, separator, email = author.rpartition(" <")

    if not separator or not email.endswith(">"):
        raise ValueError("author must be in the format 'Name <email>'")

    return name, email[:-1]


def get_commit_parent(commit: str):
    parents = run_git(["rev-list", "--parents", "-n", "1", commit]).split()
    return parents[1] if len(parents) > 1 else None


def add_trailers(message: str, trailers: list[str]) -> str:
    if not trailers:
        return message

    cmd = ["interpret-trailers"]
    for trailer in trailers:
        cmd.extend(["--trailer", trailer])

    result = subprocess.run(
        ["git", *cmd],
        input=message,
        capture_output=True,
        text=True,
        check=True
    )

    return result.stdout


def get_signoff_trailer() -> str:
    ident = run_git(["var", "GIT_COMMITTER_IDENT"])
    name, rest = ident.rsplit(" <", 1)
    email = rest.split(">", 1)[0]
    return f"Signed-off-by: {name} <{email}>"


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


def finalize_commit(content_before_trailer: str, trailer: str, expected_hash: str, old_head=None):
    """Write the winning commit object for real and move the current branch to it."""
    full_content = content_before_trailer + trailer

    actual_hash = run_git(["hash-object", "-w", "-t", "commit", "--stdin"], input_text=full_content)

    if actual_hash != expected_hash:
        sys.exit(
            f"error: mismatch between predicted hash ({expected_hash}) and what git "
            f"actually wrote ({actual_hash}) - refusing to move the branch. "
            f"(Likely cause: working tree/index changed between search and finalize.)"
        )

    branch_check = subprocess.run(
        ["git", "symbolic-ref", "--short", "-q", "HEAD"],
        capture_output=True, text=True,
    )
    if branch_check.returncode != 0:
        sys.exit("error: HEAD is not on a branch (detached HEAD?) - not moving anything automatically.")

    branch = branch_check.stdout.strip()
    ref = f"refs/heads/{branch}"
    run_git(["update-ref", ref, actual_hash, old_head or ""])
    print(f"done: {branch} now points to {actual_hash}")


def confirm(prompt: str) -> bool:
    """Ask a y/n question; only an explicit 'y' counts as yes."""
    answer = input(f"{prompt} [y/N] ").strip().lower()
    return answer == "y"


def main():
    parser = argparse.ArgumentParser()
    message_group = parser.add_mutually_exclusive_group()
    message_group.add_argument("-m", "--message", action="append", help="commit message (can be specified multiple times)")
    message_group.add_argument("-F", "--file", help="read commit message from file")
    message_group.add_argument("-C", dest="commit", metavar="commit", help="reuse message and author from specified commit")
    message_group.add_argument("-c", dest="commit_edit", metavar="commit", help="reuse message and author from specified commit, then edit")
    parser.add_argument("-a", "--all", action="store_true", help="stage all modified and deleted tracked files")
    parser.add_argument("--allow-empty", action="store_true", help="allow creating a commit with no changes")
    parser.add_argument("--allow-empty-message", action="store_true", help="allow creating a commit with an empty commit message")
    parser.add_argument("target", help="desired lowercase hex prefix for the commit hash")
    parser.add_argument("-s", "--signoff", action="store_true", help="add a Signed-off-by trailer")
    parser.add_argument("--trailer", action="append", help="add a commit trailer")
    parser.add_argument("--amend", action="store_true", help="replace the tip of the current branch")
    parser.add_argument("--author", help="override commit author")
    parser.add_argument("--date", help="override author date")
    args = parser.parse_args()

    # Capture HEAD before doing anything that could take a long time.
    head_check = subprocess.run(
        ["git", "rev-parse", "--verify", "-q", "HEAD"],
        capture_output=True, text=True,
    )
    old_head = head_check.stdout.strip() if head_check.returncode == 0 else None

    if args.amend and old_head is None:
        sys.exit("error: cannot amend; there is no existing commit")

    if args.all:
        run_git(["add", "-u"])

    if not args.allow_empty and not args.amend:
        staged = subprocess.run(["git", "diff", "--cached", "--quiet"])
        if staged.returncode == 0:
            print(run_git(["status"]))
            return
        elif staged.returncode != 1:
            sys.exit("error: failed to check staged changes")

    # Select the commit whose message/author metadata we want to reuse.
    source_commit = args.commit or args.commit_edit

    if args.amend and source_commit is None:
        source_commit = old_head

    metadata = get_commit_metadata(source_commit) if source_commit else None

    # Select the commit message.
    if args.file:
        message = Path(args.file).read_text()
    elif args.message is not None:
        message = "\n\n".join(args.message)
    elif args.commit:
        message = metadata["message"]
    elif metadata is not None:
        message = get_commit_message(metadata["message"])
    else:
        message = get_commit_message()

    if message.strip() == '' and not args.allow_empty_message:
        print("Aborting commit due to empty commit message.")
        return

    # Select author metadata.
    author = metadata["author"] if metadata else None
    date = metadata["date"] if metadata else None

    if args.author:
        author = parse_author(args.author)

    if args.date:
        date = args.date

    # Select parent.
    parent = get_commit_parent(old_head) if args.amend else AUTO_PARENT

    trailers = args.trailer or []
    if args.signoff:
        trailers.append(get_signoff_trailer())
    trailers.append("Vanity: ")
    message = add_trailers(message, trailers)[:-1]

    content = build_commit_content(
        message,
        parent=parent,
        author=author,
        date=date,
    )
    print('') # newline for aesthetic purpose only
    result = run_backend(content, args.target)

    print(f"found: {result['hash']}")
    print(f"Vanity: {result['trailer']}")
    trailer = f"{result['trailer']}"
    if not confirm(f"Commit as {result['hash']}?"):
        print("aborted, nothing was written.")
        return
    finalize_commit(content, trailer, result["hash"], old_head)


if __name__ == "__main__":
    main()

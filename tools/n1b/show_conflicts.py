"""Print the conflict hunks `git merge-tree` produces for two commits, to see what conflicts.

show_conflicts.py <repo> <ours> <theirs> [--files N] [--hunks N]
"""

import subprocess
import sys


def git(repo: str, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", repo, *args],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    ).stdout


def main() -> None:
    repo, ours, theirs = sys.argv[1:4]
    max_files = int(sys.argv[sys.argv.index("--files") + 1]) if "--files" in sys.argv else 3
    max_hunks = int(sys.argv[sys.argv.index("--hunks") + 1]) if "--hunks" in sys.argv else 2
    out = git(repo, "merge-tree", "--write-tree", "--name-only", "--no-messages", ours, theirs)
    lines = out.splitlines()
    tree, files = lines[0], [name for name in lines[1:] if name.strip()]
    print(f"tree {tree}: {len(files)} conflicted files")
    for f in files[:max_files]:
        text = git(repo, "show", f"{tree}:{f}").splitlines()
        print(f"=== {f}")
        n = 0
        i = 0
        while i < len(text) and n < max_hunks:
            if text[i].startswith("<<<<<<<"):
                j = i
                while j < len(text) and not text[j].startswith(">>>>>>>"):
                    j += 1
                for line in text[max(0, i - 1) : j + 1]:
                    print("   ", line[:170])
                n += 1
                i = j
            i += 1


if __name__ == "__main__":
    main()

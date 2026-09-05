"""Count every first-party production C/C++ line, including private hardware code."""
import argparse
import json
import re
import subprocess
from pathlib import Path

TOKENS = re.compile(r"""//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'""")


def effective(source):
    def strip(match):
        text = match.group()
        return "\n" * text.count("\n") if text.startswith(("//", "/*")) else text
    return sum(bool(line.strip()) for line in TOKENS.sub(strip, source).splitlines())


def source_metrics(source):
    # 这些指标辅助检查阅读负担；不把行数当作代码质量分数。
    code = TOKENS.sub(
        lambda match: "\n" * match.group().count("\n")
        if match.group().startswith(("//", "/*")) else '""',
        source,
    )
    return dict(
        physical=len(source.splitlines()),
        eloc=effective(source),
        conditional_groups=len(re.findall(r"^\s*#\s*(?:if|ifdef|ifndef)\b", code, re.M)),
        multi_statement_lines=sum(
            line.count(";") > 1 and not re.search(r"\bfor\s*\(", line)
            for line in code.splitlines()
        ),
    )


def measure(root, revision=None):
    rows = []
    directories = ("client/apps/boompi_client", "client/src", "client/include")
    if revision:
        names = subprocess.check_output(
            ["git", "ls-tree", "-r", "--name-only", revision, "--", *directories],
            cwd=root, text=True,
        ).splitlines()
    else:
        names = [
            path.relative_to(root).as_posix()
            for directory in directories
            for path in (root / directory).rglob("*")
            if path.is_file()
        ]
    for name in sorted(names):
        if Path(name).suffix not in (".cpp", ".h", ".c", ".hpp", ".cc"):
            continue
        if revision:
            source = subprocess.check_output(
                ["git", "show", f"{revision}:{name}"], cwd=root,
            ).decode("utf-8")
        else:
            source = (root / name).read_text(encoding="utf-8")
        rows.append(dict(file=name, **source_metrics(source)))
    return dict(files=len(rows), physical=sum(r["physical"] for r in rows),
                eloc=sum(r["eloc"] for r in rows),
                conditional_groups=sum(r["conditional_groups"] for r in rows),
                multi_statement_lines=sum(r["multi_statement_lines"] for r in rows),
                by_file=rows)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--revision", help="Compare the same source roots at a Git revision")
    args = parser.parse_args()
    print(json.dumps(measure(args.root.resolve(), args.revision), ensure_ascii=False, indent=2))

"""Count every first-party production C/C++ line, including private hardware code."""
import argparse
import json
import re
from pathlib import Path

TOKENS = re.compile(r"""//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'""")


def effective(source):
    def strip(match):
        text = match.group()
        return "\n" * text.count("\n") if text.startswith(("//", "/*")) else text
    return sum(bool(line.strip()) for line in TOKENS.sub(strip, source).splitlines())


def measure(root):
    rows = []
    for directory in ("client/apps/boompi_client", "client/src", "client/include"):
        for path in sorted((root / directory).rglob("*")):
            if path.suffix not in (".cpp", ".h", ".c", ".hpp", ".cc"):
                continue
            source = path.read_text(encoding="utf-8")
            rows.append(dict(file=path.relative_to(root).as_posix(),
                             physical=len(source.splitlines()), eloc=effective(source)))
    return dict(files=len(rows), physical=sum(r["physical"] for r in rows),
                eloc=sum(r["eloc"] for r in rows), by_file=rows)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    print(json.dumps(measure(args.root.resolve()), ensure_ascii=False, indent=2))

#!/usr/bin/env python3
"""Verify whether an ELF binary matches the boomPI RV1106 runtime ABI."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Dict, List, Optional, Sequence, Tuple


EXPECTED_INTERPRETER = "/lib/ld-uClibc.so.0"
READELF_TIMEOUT_SECONDS = 10
GLIBCXX_PATTERN = re.compile(r"\bGLIBCXX_(\d+(?:\.\d+)*)\b")
DEVELOPMENT_PATH_PATTERN = re.compile(
    rb"(?:"
    rb"/(?:home|Users|root|tmp|workspace|workspaces|build|builds|mnt|media|Volumes)/"
    rb"|/private/(?:tmp|var)/"
    rb"|/opt/(?:sdk|toolchains?|build)/"
    rb"|[A-Za-z]:[\\/](?:Users|workspace|workspaces|build|builds|temp)[\\/]"
    rb")",
    re.IGNORECASE,
)


class VerificationToolError(Exception):
    """A stable, path-free description of a verifier/tool failure."""

    def __init__(self, code: str, stage: str) -> None:
        super().__init__(code)
        self.code = code
        self.stage = stage


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Validate a boomPI binary against the RV1106 uClibc ABI.",
    )
    parser.add_argument("elf", help="ELF file to inspect")
    parser.add_argument(
        "--readelf",
        default=os.environ.get("READELF", "readelf"),
        help="readelf executable (or a Python .py test shim)",
    )
    parser.add_argument(
        "--max-glibcxx",
        metavar="VERSION",
        help="maximum allowed GLIBCXX version, for example GLIBCXX_3.4.9",
    )
    return parser


def _normalize_glibcxx(value: str) -> Tuple[str, Tuple[int, ...]]:
    token = value if value.startswith("GLIBCXX_") else "GLIBCXX_" + value
    match = re.fullmatch(r"GLIBCXX_(\d+(?:\.\d+)*)", token)
    if match is None:
        raise ValueError("invalid GLIBCXX ceiling")
    version = tuple(int(component) for component in match.group(1).split("."))
    return token, version


def _readelf_command(readelf: str) -> List[str]:
    # Supporting a Python shim keeps offline tests portable without invoking a shell.
    if Path(readelf).suffix.lower() == ".py":
        return [sys.executable, readelf]
    return [readelf]


def _run_readelf(readelf: str, option: str, elf: Path, stage: str) -> str:
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    environment["LANG"] = "C"
    command = _readelf_command(readelf) + ["--wide", option, os.fspath(elf)]
    try:
        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            encoding="utf-8",
            errors="replace",
            env=environment,
            timeout=READELF_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired):
        raise VerificationToolError("readelf_unavailable", stage) from None

    if completed.returncode != 0:
        # Do not forward readelf stderr: it commonly contains the inspected path.
        raise VerificationToolError("readelf_failed", stage)
    return completed.stdout


def _header_value(header: str, field: str) -> Optional[str]:
    match = re.search(
        rf"^\s*{re.escape(field)}:\s*(.*?)\s*$",
        header,
        flags=re.MULTILINE,
    )
    return match.group(1) if match is not None else None


def _interpreter(program_headers: str) -> Optional[str]:
    match = re.search(
        r"Requesting program interpreter:\s*([^\]\r\n]+)",
        program_headers,
    )
    return match.group(1).strip() if match is not None else None


def _glibcxx_versions(version_info: str) -> List[Tuple[int, ...]]:
    versions = set()
    for match in GLIBCXX_PATTERN.finditer(version_info):
        versions.add(tuple(int(component) for component in match.group(1).split(".")))
    return sorted(versions)


def _version_greater(left: Tuple[int, ...], right: Tuple[int, ...]) -> bool:
    width = max(len(left), len(right))
    return left + (0,) * (width - len(left)) > right + (0,) * (width - len(right))


def _format_glibcxx(version: Optional[Tuple[int, ...]]) -> Optional[str]:
    if version is None:
        return None
    return "GLIBCXX_" + ".".join(str(component) for component in version)


def _file_contains_development_path(elf: Path) -> bool:
    overlap = 512
    previous = b""
    try:
        with elf.open("rb") as stream:
            while True:
                chunk = stream.read(1024 * 1024)
                if not chunk:
                    return DEVELOPMENT_PATH_PATTERN.search(previous) is not None
                combined = previous + chunk
                if DEVELOPMENT_PATH_PATTERN.search(combined) is not None:
                    return True
                previous = combined[-overlap:]
    except OSError:
        raise VerificationToolError("input_unreadable", "input") from None


def _safe_observed(actual: Optional[str], expected: str) -> Optional[str]:
    if actual is None:
        return None
    return expected if actual == expected else "unexpected"


def verify(elf: Path, readelf: str, max_glibcxx: Optional[str]) -> Dict[str, object]:
    if not elf.is_file():
        raise VerificationToolError("input_unreadable", "input")

    limit_token: Optional[str] = None
    limit_version: Optional[Tuple[int, ...]] = None
    if max_glibcxx is not None:
        limit_token, limit_version = _normalize_glibcxx(max_glibcxx)

    header = _run_readelf(readelf, "-h", elf, "elf_header")
    program_headers = _run_readelf(readelf, "-l", elf, "program_headers")
    dynamic = _run_readelf(readelf, "-d", elf, "dynamic_section")
    version_info = ""
    if limit_version is not None:
        version_info = _run_readelf(readelf, "--version-info", elf, "version_info")

    elf_class = _header_value(header, "Class")
    data_encoding = _header_value(header, "Data") or ""
    machine = _header_value(header, "Machine")
    flags = _header_value(header, "Flags") or ""
    interpreter = _interpreter(program_headers)

    class_ok = elf_class == "ELF32"
    little_endian_ok = "little endian" in data_encoding.lower()
    machine_ok = machine == "ARM"
    eabi5_ok = re.search(r"(?:Version5\s+EABI|\bEABI5\b)", flags) is not None
    hard_float_ok = "hard-float ABI" in flags
    interpreter_ok = interpreter == EXPECTED_INTERPRETER
    rpath_present = re.search(r"\((?:RPATH|RUNPATH)\)", dynamic) is not None
    glibc_present = re.search(r"\blibc\.so\.6\b", dynamic) is not None
    libstdcxx_present = re.search(r"\blibstdc\+\+\.so(?:\.6)?\b", dynamic) is not None

    path_scan = "\n".join((header, program_headers, dynamic, version_info)).encode(
        "utf-8", errors="replace"
    )
    development_path_present = (
        DEVELOPMENT_PATH_PATTERN.search(path_scan) is not None
        or _file_contains_development_path(elf)
    )

    versions = _glibcxx_versions(version_info)
    observed_glibcxx = versions[-1] if versions else None
    glibcxx_ok = (
        limit_version is None
        or not libstdcxx_present
        or (
            observed_glibcxx is not None
            and not _version_greater(observed_glibcxx, limit_version)
        )
    )

    checks: Dict[str, object] = {
        "elf_class": {
            "expected": "ELF32",
            "observed": elf_class if elf_class in ("ELF32", "ELF64") else "unknown",
            "ok": class_ok,
        },
        "endianness": {
            "expected": "little",
            "observed": (
                "little"
                if little_endian_ok
                else "big"
                if "big endian" in data_encoding.lower()
                else "unknown"
            ),
            "ok": little_endian_ok,
        },
        "machine": {
            "expected": "ARM",
            "observed": "ARM" if machine_ok else "other",
            "ok": machine_ok,
        },
        "eabi": {
            "expected": "EABI5",
            "observed": "EABI5" if eabi5_ok else "other_or_missing",
            "ok": eabi5_ok,
        },
        "float_abi": {
            "expected": "hard",
            "observed": "hard" if hard_float_ok else "soft_or_unspecified",
            "ok": hard_float_ok,
        },
        "interpreter": {
            "expected": EXPECTED_INTERPRETER,
            "observed": _safe_observed(interpreter, EXPECTED_INTERPRETER),
            "ok": interpreter_ok,
        },
        "rpath_runpath": {"present": rpath_present, "ok": not rpath_present},
        "absolute_development_path": {
            "present": development_path_present,
            "ok": not development_path_present,
        },
        "glibc_libc_so_6": {"present": glibc_present, "ok": not glibc_present},
        "max_glibcxx": {
            "enabled": limit_version is not None,
            "libstdcxx_present": libstdcxx_present,
            "limit": limit_token,
            "observed": _format_glibcxx(observed_glibcxx),
            "ok": glibcxx_ok,
        },
    }
    failed_checks = [
        name
        for name, details in checks.items()
        if isinstance(details, dict) and details.get("ok") is False
    ]
    return {
        "schema_version": 1,
        "compatible": not failed_checks,
        "failed_checks": failed_checks,
        "checks": checks,
    }


def _emit(document: Dict[str, object]) -> None:
    json.dump(document, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")


def main(arguments: Optional[Sequence[str]] = None) -> int:
    parser = _build_parser()
    parsed = parser.parse_args(arguments)
    try:
        # Parsing here lets invalid ceilings retain the documented usage-error code.
        if parsed.max_glibcxx is not None:
            _normalize_glibcxx(parsed.max_glibcxx)
        document = verify(Path(parsed.elf), parsed.readelf, parsed.max_glibcxx)
    except ValueError:
        parser.error("--max-glibcxx must be GLIBCXX_N.N[.N] or N.N[.N]")
    except VerificationToolError as error:
        _emit(
            {
                "schema_version": 1,
                "compatible": False,
                "error": {"code": error.code, "stage": error.stage},
            }
        )
        return 2

    _emit(document)
    return 0 if document["compatible"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

"""Run the project's pinned quality tools without traversing upstream repositories."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import subprocess
import sys
import venv
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENVIRONMENT = ROOT / "build/quality-tools"
BIN = ENVIRONMENT / "bin"
UI = ROOT / "build/jetkvm/ui"
SOURCE_DIRECTORIES = ("native", "service", "tools", "tv-app", "cmake", ".github")
WEB_EXTENSIONS = {
    ".mjs",
    ".js",
    ".jsx",
    ".ts",
    ".tsx",
    ".json",
    ".jsonc",
    ".css",
    ".html",
    ".md",
    ".yml",
    ".yaml",
}


def run(*arguments: str | Path) -> None:
    subprocess.run([str(argument) for argument in arguments], cwd=ROOT, check=True)


def tool(name: str) -> Path:
    executable = BIN / name
    if not executable.is_file():
        raise RuntimeError("Run npm run quality:setup to install the pinned quality tools")
    return executable


def files() -> list[Path]:
    selected = [
        path
        for directory in SOURCE_DIRECTORIES
        for path in (ROOT / directory).rglob("*")
        if path.is_file() and "__pycache__" not in path.parts
    ]
    selected += [
        ROOT / name
        for name in (
            "README.md",
            "package.json",
            "package-lock.json",
            "appinfo.json",
            "services.json",
            "CMakeLists.txt",
            "autostart.sh",
            ".oxfmtrc.json",
            ".oxlintrc.json",
            ".oxlintrc.ui.json",
        )
    ]
    selected += [ROOT / "patches/jetkvm/README.md"]
    if not (UI / "src/webos").is_dir():
        raise RuntimeError("Run npm run prepare:ui before checking the frontend integration")
    selected += list((UI / "src/webos").rglob("*"))
    return sorted({path for path in selected if path.is_file()})


def format_sources(check: bool) -> None:
    selected = files()
    native = [path for path in selected if path.suffix in {".c", ".cpp", ".hpp"}]
    run(tool("clang-format"), *(["--dry-run", "--Werror"] if check else ["-i"]), *native)
    web = [path for path in selected if path.suffix in WEB_EXTENSIONS]
    run(
        ROOT / "node_modules/.bin/oxfmt",
        "--config",
        ROOT / ".oxfmtrc.json",
        "--ignore-path",
        ROOT / ".oxfmtignore",
        "--check" if check else "--write",
        *web,
    )
    python = [path for path in selected if path.suffix == ".py"]
    run(tool("ruff"), "format", *(["--check"] if check else []), *python)
    cmake = [path for path in selected if path.name == "CMakeLists.txt" or path.suffix == ".cmake"]
    run(
        tool("gersemi"),
        "-l",
        "100",
        "--indent",
        "4",
        "--warnings-as-errors",
        "--definitions",
        ROOT / "native/stubs/CMakeLists.txt",
        "--check" if check else "--in-place",
        *cmake,
    )

    shell = [path for path in selected if path.suffix == ".sh"]
    run(tool("shfmt"), "-ln", "posix", "-i", "4", "-d" if check else "-w", *shell)


def lint() -> None:
    selected = files()
    javascript = [path for path in selected if path.suffix in {".js", ".mjs"}]
    run(
        ROOT / "node_modules/.bin/oxlint",
        "--config",
        ROOT / ".oxlintrc.json",
        "--no-ignore",
        "--disable-nested-config",
        "--deny-warnings",
        *javascript,
    )
    integration = [path for path in selected if path.suffix in {".ts", ".tsx"}]
    run(
        ROOT / "node_modules/.bin/oxlint",
        "--config",
        ROOT / ".oxlintrc.ui.json",
        "--no-ignore",
        "--disable-nested-config",
        "--type-aware",
        "--tsconfig",
        UI / "tsconfig.app.json",
        "--deny-warnings",
        *integration,
    )
    run(tool("ruff"), "check", *(path for path in selected if path.suffix == ".py"))
    run(tool("shellcheck"), "--shell=sh", *(path for path in selected if path.suffix == ".sh"))


def tidy(directory: Path) -> None:
    database = directory.resolve() / "compile_commands.json"
    if not database.is_file():
        raise RuntimeError(f"Configure {directory} with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON first")
    commands = json.loads(database.read_text())
    selected = {}
    for entry in commands:
        source = Path(entry["file"]).resolve()
        if source.is_relative_to(ROOT) and source.relative_to(ROOT).parts[0] in {
            "native",
            "service",
            "tools",
        }:
            selected.setdefault(source, entry)
    sources = sorted(selected)
    # Analyze each translation unit once, even when shared by several test targets.
    analysis_directory = ROOT / "build/quality-native" / directory.resolve().name
    analysis_directory.mkdir(parents=True, exist_ok=True)
    (analysis_directory / "compile_commands.json").write_text(json.dumps(list(selected.values())))
    extra_arguments = []
    cross_compiling = any("--sysroot" in entry.get("command", "") for entry in selected.values())
    if sys.platform == "darwin" and not cross_compiling:
        # PyPI's LLVM binaries need the C++ headers supplied by Xcode's toolchain.
        compiler = Path(subprocess.check_output(["xcrun", "--find", "clang++"], text=True).strip())
        sdk = Path(subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip())
        headers = compiler.parent.parent / "include/c++/v1"
        if not headers.is_dir():
            headers = sdk / "usr/include/c++/v1"
        if not headers.is_dir():
            raise RuntimeError("Cannot locate Xcode's libc++ headers")
        extra_arguments = [f"--extra-arg=-isystem{headers}", f"--extra-arg=-isysroot{sdk}"]

    def analyze(source: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                str(tool("clang-tidy")),
                "--quiet",
                "-p",
                str(analysis_directory),
                *extra_arguments,
                str(source),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )

    failures = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(4, os.cpu_count() or 1)) as executor:
        for source, result in zip(
            sorted(sources), executor.map(analyze, sorted(sources)), strict=True
        ):
            if result.returncode:
                print(f"{source.relative_to(ROOT)}:\n{result.stdout}{result.stderr}")
                failures += 1
    if failures:
        raise RuntimeError(f"clang-tidy failed for {failures} translation units")
    print(f"clang-tidy passed for {len(sources)} first-party translation units")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("setup", "format", "check-format", "lint", "tidy"))
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/host")
    args = parser.parse_args()
    if args.action == "setup":
        if not (BIN / "python").is_file():
            venv.create(ENVIRONMENT, with_pip=True, symlinks=True)
        elif subprocess.run(
            [str(BIN / "python"), "-m", "pip", "--version"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        ).returncode:
            run(BIN / "python", "-m", "ensurepip", "--upgrade")
        run(
            BIN / "python",
            "-m",
            "pip",
            "install",
            "--require-hashes",
            "-r",
            ROOT / "tools/requirements-quality.txt",
        )
    elif args.action in {"format", "check-format"}:
        format_sources(args.action == "check-format")
    elif args.action == "lint":
        lint()
    else:
        tidy(args.build_dir)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)

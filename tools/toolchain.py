"""Find the symbol tool for an existing Zephyr build without local SDK paths."""

import shutil
from pathlib import Path


def resolve_nm(elf: Path, override: str | None = None) -> str:
    """Prefer an explicit tool, then the build's CMake metadata, then PATH."""
    if override is not None:
        executable = shutil.which(override)
        if executable:
            return executable
        raise FileNotFoundError(f"--nm tool is not executable or not found: {override}")

    cache = elf.parent.parent / "CMakeCache.txt"
    try:
        lines = cache.read_text(encoding="utf-8").splitlines()
    except OSError:
        lines = []
    for line in lines:
        key, separator, value = line.partition("=")
        if separator and key.split(":", 1)[0] == "CMAKE_NM":
            executable = shutil.which(value.strip())
            if executable:
                return executable

    executable = shutil.which("arm-zephyr-eabi-nm")
    if executable:
        return executable
    raise FileNotFoundError(
        f"Cannot find the ARM nm tool in {cache} or on PATH. "
        "Select an existing Zephyr build or pass --nm /path/to/arm-zephyr-eabi-nm."
    )

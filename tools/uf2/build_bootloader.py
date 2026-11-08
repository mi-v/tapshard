#!/usr/bin/env python3
"""Build the independent, USB-free nRF52840 bootloader with the Zephyr compiler."""
import argparse
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path


def build(cc: Path, objcopy: Path, output: Path, unlocked: bool):
    root = Path(__file__).resolve().parents[2]
    source = root / "bootloader"
    output.mkdir(parents=True, exist_ok=True)
    version = subprocess.run([str(cc), "-dumpfullversion"], check=True,
                             capture_output=True, text=True, timeout=10).stdout.strip()
    if version != "14.3.0":
        raise ValueError(f"unreviewed compiler {version}; expected Zephyr SDK GCC 14.3.0")
    elf = output / "bootloader.elf"
    command = [str(cc), "-mcpu=cortex-m4", "-mthumb", "-mabi=aapcs", "-Os",
               "-std=c99", "-Wall", "-Wextra", "-Werror", "-g3", "-ffreestanding",
               "-fno-builtin", "-fno-stack-protector", "-fno-common",
               "-fno-tree-loop-distribute-patterns", "-fdata-sections",
               "-ffunction-sections", "-nostdlib", f"-DTOTP_BOOT_UNLOCKED={int(unlocked)}",
               "-T", str(source / "linker.ld"), "-Wl,--gc-sections,--build-id=none",
               f"-Wl,-Map,{output / 'bootloader.map'}", "-o", str(elf)]
    command += [str(source / name) for name in
                ("startup.S", "main.c", "boot.c", "factory.c", "sha256.c", "svc.S", "runtime.c")]
    subprocess.run(command + ["-lgcc"], check=True, timeout=60)
    objdump = objcopy.with_name(objcopy.name.replace("objcopy", "objdump"))
    nm = objcopy.with_name(objcopy.name.replace("objcopy", "nm"))
    symbol_text = subprocess.run([str(nm), "-n", str(elf)], check=True,
                                 capture_output=True, text=True, timeout=10).stdout
    symbols = {parts[2]: int(parts[0], 16) for line in symbol_text.splitlines()
               if len(parts := line.split()) == 3}
    disassembly = subprocess.run([str(objdump), "-d", "--disassemble=Reset_Handler", str(elf)],
                                check=True, capture_output=True, text=True, timeout=10).stdout
    # In protected builds wipe_done and after_wipe share an address, and
    # objdump may print either alias. Bound instructions by the ELF symbol.
    early = "\n".join(line for line in disassembly.splitlines()
                       if (match := re.match(r"\s*([0-9a-f]+):", line)) and
                       symbols["Reset_Handler"] <= int(match[1], 16) < symbols["boot_wipe_done"])
    # A call, stack access, or interrupt enable before wipe end would expose
    # retained secrets or overwrite stack state while the wipe is in progress.
    if not early or not re.search(r"\bcpsid\s+i\b", early.splitlines()[0]) or re.search(
            r"\b(?:push|pop|bl|blx|cpsie|sp|msp|psp)\b", early, re.IGNORECASE):
        raise ValueError("reset handler uses stack/calls/interrupts before SRAM wipe")
    if ("boot_wipe_wait" in symbols) != unlocked:
        raise ValueError("bench wipe stop must exist only in unlocked builds")
    # GCC's default specs supply picolibc.ld unless -T is passed to GCC itself.
    # Verify load addresses before objcopy can materialize a huge sparse file.
    image = elf.read_bytes()
    phoff = struct.unpack_from("<I", image, 28)[0]
    phsize, phcount = struct.unpack_from("<HH", image, 42)
    for index in range(phcount):
        kind, _, _, address, size = struct.unpack_from("<5I", image, phoff + index * phsize)
        if kind == 1 and size and not 0xF4000 <= address < address + size <= 0xFE000:
            raise ValueError(f"bootloader load segment outside reserved region: {address:#x}")
    binary = output / "bootloader.bin"
    binary.unlink(missing_ok=True)
    subprocess.run([str(objcopy), "-O", "binary", "--gap-fill", "0xff",
                    "--only-section=.vectors", "--only-section=.boot_id",
                    "--only-section=.text", str(elf),
                    str(binary)], check=True, timeout=10)
    if not 0 < binary.stat().st_size <= 0xA000:
        raise ValueError("bootloader exceeds the 40 KiB reserved region")
    data = binary.read_bytes()
    if not 0 < len(data) <= 0xA000:
        raise ValueError("bootloader exceeds the 40 KiB reserved region")
    data += b"\xff" * (0xA000 - len(data))
    stack, reset = struct.unpack_from("<2I", data)
    if stack != 0x2003FFF0 or not reset & 1 or not 0xF4110 <= reset < 0xFE000:
        raise ValueError("invalid bootloader stack/reset vectors")
    if struct.unpack_from("<4I", data, 0x100) != (0x534F5450, 2, int(unlocked), 0xF4000):
        raise ValueError("invalid bootloader identity")
    binary.write_bytes(data)
    digest = hashlib.sha256(data).digest()
    (output / "bootloader_payload.S").write_text(
        '.section .rodata.boot_payload,"a",%progbits\n'
        '.balign 4096\n.global totp_boot_payload\ntotp_boot_payload:\n'
        f'.incbin "{binary.resolve()}"\n'
        '.global totp_boot_digest\ntotp_boot_digest:\n.byte ' +
        ','.join(str(byte) for byte in digest) + '\n')
    inputs = sorted(source.glob("*")) + [Path(__file__).resolve()]
    source_hashes = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in inputs if path.is_file()}
    (output / "bootloader.json").write_text(json.dumps({
        "compiler_version": version,
        "compiler_sha256": hashlib.sha256(cc.read_bytes()).hexdigest(),
        "unlocked": unlocked, "sha256": digest.hex(), "sources": source_hashes,
    }, indent=2, sort_keys=True) + "\n")
    print(f"Bootloader: {len(data)} bytes, unlocked={unlocked}, SHA256={digest.hex()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", type=Path, required=True)
    parser.add_argument("--objcopy", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--unlocked", action="store_true")
    args = parser.parse_args()
    build(args.cc, args.objcopy, args.output, args.unlocked)


if __name__ == "__main__":
    main()

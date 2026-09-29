"""Run a relinked Linux executable whose AMD-only instruction was replaced by a stub."""

from pathlib import Path
import platform
import struct
import subprocess
import sys
import tempfile

from test_linux_load_alignment import fixture

CODE_VADDR = 0x10
DATA_VADDR = 0x100
CODE_OFFSET = 0x4000 + CODE_VADDR
DATA_OFFSET = 0x4000 + DATA_VADDR
EXPECTED = 42


def payload():
    """Extract a byte field with the register form of EXTRQ and exit with it."""
    lea = b"\x48\x8d\x35"
    lea_end = CODE_VADDR + len(lea) + 4
    code = lea + struct.pack("<i", DATA_VADDR - lea_end)
    code += b"\xf3\x0f\x6f\x06"          # movdqu xmm0, [rsi]
    code += b"\xf3\x0f\x6f\x4e\x10"      # movdqu xmm1, [rsi+16]
    code += b"\x66\x0f\x79\xc1"          # extrq  xmm0, xmm1   (AMD only, register form)
    code += b"\x66\x48\x0f\x7e\xc7"      # movq   rdi, xmm0
    code += b"\xb8\xe7\x00\x00\x00"      # mov    eax, 231     (exit_group)
    code += b"\x0f\x05"                  # syscall
    return code


def source():
    image = fixture()
    struct.pack_into("<Q", image, 24, CODE_VADDR)
    code = payload()
    image[CODE_OFFSET:CODE_OFFSET + len(code)] = code
    # The field sits in bits 15:8, so a length of 8 and an index of 8 extract it.
    struct.pack_into("<QQ", image, DATA_OFFSET, EXPECTED << 8, 0)
    struct.pack_into("<QQ", image, DATA_OFFSET + 16, 8 | (8 << 8), 0)
    return image


def segments(elf):
    phoff, = struct.unpack_from("<Q", elf, 32)
    phentsize, phnum = struct.unpack_from("<HH", elf, 54)
    return [struct.unpack_from("<IIQQQQQQ", elf, phoff + index * phentsize) for index in range(phnum)]


def read_vaddr(elf, vaddr, size):
    for kind, _, offset, segment, _, filesz, _, _ in segments(elf):
        if kind == 1 and segment <= vaddr < segment + filesz:
            start = offset + vaddr - segment
            return elf[start:start + size]
    raise AssertionError(f"virtual address {vaddr:#x} is not mapped")


def main():
    relinker = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="anyps5-stub-exec-") as directory:
        work = Path(directory)
        elf = work / "input.elf"
        output = work / "output.elf"
        elf.write_bytes(source())
        result = subprocess.run([str(relinker), "--skip-sce-module", "--skip-syscall-check", "--to-intel",
                                 str(elf), str(output)], capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert "EXTRQ register form" in result.stdout, result.stdout

        relinked = output.read_bytes()
        site = read_vaddr(relinked, CODE_VADDR + 16, 5)
        assert site[0] == 0xE9, "the AMD-only site was not replaced by a jump"
        target = CODE_VADDR + 21 + struct.unpack_from("<i", site, 1)[0]
        stub = read_vaddr(relinked, target, 0x200)
        assert stub[0] == 0x48, "the stub does not start with the red-zone skip"
        executable = [header for header in segments(relinked)
                      if header[0] == 1 and header[1] & 1 and header[3] <= target < header[3] + header[6]]
        assert executable, "the stub is not inside an executable segment"

        if platform.system() == "Linux" and platform.machine() == "x86_64":
            output.chmod(0o755)
            run = subprocess.run([str(output)], capture_output=True, timeout=60)
            assert run.returncode == EXPECTED, f"the relinked executable returned {run.returncode}"
            print("Linux stub execution tests passed (executed)")
            return
    print("Linux stub execution tests passed (inspection only)")


if __name__ == "__main__":
    main()

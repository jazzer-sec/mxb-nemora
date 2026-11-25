"""Read the running game's unpacked code and report candidate function RVAs.

Usage: python tools/scan_offsets.py <mxbikes PID>
This tool reads process memory only and does not patch the game or the mod.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import re
import struct
import sys


def scan(data, pattern):
    parts = pattern.split()
    if not parts or all(part == '??' for part in parts):
        raise ValueError('empty or all-wildcard pattern')
    regex = b''.join(b'.' if part == '??' else re.escape(bytes.fromhex(part)) for part in parts)
    return [match.start() for match in re.finditer(b'(?=(' + regex + b'))', data, re.DOTALL)]


def read_text(pid):
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    psapi = ctypes.WinDLL('psapi', use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    kernel.ReadProcessMemory.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    psapi.EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
    psapi.EnumProcessModulesEx.restype = wintypes.BOOL
    handle = kernel.OpenProcess(0x410, False, pid)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        modules = (wintypes.HMODULE * 1024)()
        needed = wintypes.DWORD()
        if not psapi.EnumProcessModulesEx(handle, modules, ctypes.sizeof(modules), ctypes.byref(needed), 3):
            raise ctypes.WinError(ctypes.get_last_error())
        base = modules[0]

        def read(address, size):
            data = ctypes.create_string_buffer(size)
            count = ctypes.c_size_t()
            if not kernel.ReadProcessMemory(handle, address, data, size, ctypes.byref(count)) or count.value != size:
                raise ctypes.WinError(ctypes.get_last_error())
            return data.raw

        header = read(base, 4096)
        if header[:2] != b'MZ':
            raise ValueError('process does not have a valid executable header')
        pe = struct.unpack_from('<I', header, 0x3c)[0]
        if pe + 24 > len(header) or header[pe:pe+4] != b'PE\0\0':
            raise ValueError('invalid PE header')
        count = struct.unpack_from('<H', header, pe+6)[0]
        optional_size = struct.unpack_from('<H', header, pe+20)[0]
        timestamp = struct.unpack_from('<I', header, pe+8)[0]
        for i in range(count):
            at = pe+24+optional_size+i*40
            if at+40 > len(header):
                raise ValueError('section table extends beyond header')
            if header[at:at+8].rstrip(b'\0') == b'.text':
                size, rva = struct.unpack_from('<II', header, at+8)
                if not 0 < size < 64 * 1024 * 1024:
                    raise ValueError('invalid text section size')
                return timestamp, rva, read(base+rva, size)
        raise ValueError('no text section')
    finally:
        kernel.CloseHandle(handle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid', type=int)
    args = parser.parse_args()
    signatures = json.loads(Path(__file__).with_name('signatures.json').read_text())
    timestamp, base_rva, code = read_text(args.pid)
    print(f'PE timestamp: 0x{timestamp:08X}; text RVA: 0x{base_rva:X}')
    failures = 0
    for name, entry in signatures.items():
        hits = scan(code, entry['pattern'])
        if len(hits) == 1:
            rva = base_rva + hits[0]
            print(f'{name}: 0x{rva:X} (beta21f {entry["beta21f_rva"]})')
        else:
            failures += 1
            print(f'{name}: {len(hits)} matches (unresolved)')
    print(f'{len(signatures)-failures}/{len(signatures)} unique matches; field layouts and semantics require verification.')
    return 1 if failures else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError) as exc:
        print(f'Scan failed: {exc}', file=sys.stderr)
        sys.exit(2)

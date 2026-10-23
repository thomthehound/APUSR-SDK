# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""Contract for the optional loader-only Streamline bootstrap proxy."""

from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXPORTS = ROOT / "src/frontends/streamline_bootstrap_proxy/apusr_streamline_bootstrap_exports.def.inc"
PROXY = ROOT / "src/frontends/streamline_bootstrap_proxy/apusr_streamline_bootstrap_proxy.cpp"
ATTACH_ABI = ROOT / "src/frontends/streamline_attach/apusr_streamline_attach_abi.h"
INSTALLER = ROOT / "packaging/streamline_bootstrap/install_apusr_streamline_bootstrap.bat"
CMAKE = ROOT / "CMakeLists.txt"


def _pe_layout(path: Path) -> tuple[bytes, int, list[tuple[int, int, int, int]]]:
    data = path.read_bytes()
    assert len(data) >= 0x40 and data[:2] == b"MZ", f"not a PE image: {path}"
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe_off : pe_off + 4] == b"PE\0\0"
    coff_off = pe_off + 4
    _, section_count, _, _, _, optional_size, _ = struct.unpack_from("<HHIIIHH", data, coff_off)
    optional_off = coff_off + 20
    magic = struct.unpack_from("<H", data, optional_off)[0]
    assert magic in (0x10B, 0x20B), f"unknown PE optional-header magic 0x{magic:x}"
    data_dir_off = optional_off + (96 if magic == 0x10B else 112)
    section_off = optional_off + optional_size
    sections: list[tuple[int, int, int, int]] = []
    for index in range(section_count):
        off = section_off + index * 40
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", data, off + 8)
        sections.append((virtual_address, virtual_size, raw_offset, raw_size))
    return data, data_dir_off, sections


def _rva_to_offset(rva: int, sections: list[tuple[int, int, int, int]]) -> int:
    for virtual_address, virtual_size, raw_offset, raw_size in sections:
        span = max(virtual_size, raw_size)
        if virtual_address <= rva < virtual_address + span:
            delta = rva - virtual_address
            assert delta < raw_size
            return raw_offset + delta
    raise AssertionError(f"RVA 0x{rva:x} is not backed by a PE section")


def _cstring(data: bytes, offset: int) -> str:
    end = data.index(b"\0", offset)
    return data[offset:end].decode("ascii")


def load_pe_export_entries(path: Path) -> dict[str, tuple[int, str | None]]:
    data, data_dir_off, sections = _pe_layout(path)
    export_rva, export_size = struct.unpack_from("<II", data, data_dir_off)
    assert export_rva and export_size
    export_off = _rva_to_offset(export_rva, sections)
    fields = struct.unpack_from("<IIHHIIIIIII", data, export_off)
    ordinal_base, function_count, number_of_names = fields[5], fields[6], fields[7]
    address_of_functions, address_of_names, address_of_name_ordinals = fields[8], fields[9], fields[10]
    functions_off = _rva_to_offset(address_of_functions, sections)
    names_off = _rva_to_offset(address_of_names, sections)
    ordinals_off = _rva_to_offset(address_of_name_ordinals, sections)
    assert function_count >= number_of_names
    exports: dict[str, tuple[int, str | None]] = {}
    for index in range(number_of_names):
        name_rva = struct.unpack_from("<I", data, names_off + 4 * index)[0]
        name = _cstring(data, _rva_to_offset(name_rva, sections))
        ordinal_index = struct.unpack_from("<H", data, ordinals_off + 2 * index)[0]
        function_rva = struct.unpack_from("<I", data, functions_off + 4 * ordinal_index)[0]
        forwarder = None
        if export_rva <= function_rva < export_rva + export_size:
            forwarder = _cstring(data, _rva_to_offset(function_rva, sections))
        exports[name] = (ordinal_base + ordinal_index, forwarder)
    return exports


def load_pe_exports(path: Path) -> dict[str, int]:
    return {name: ordinal for name, (ordinal, _) in load_pe_export_entries(path).items()}

def parse_exports() -> dict[str, int]:
    exports: dict[str, int] = {}
    pattern = re.compile(r"^\s*([A-Za-z0-9_]+)\s+@(\d+)\s*$")
    for line in EXPORTS.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if not match:
            continue
        name, ordinal = match.group(1), int(match.group(2))
        exports[name] = ordinal
    return exports


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dll", type=Path)
    parser.add_argument("--stock-dll", type=Path, action="append", default=[])
    args = parser.parse_args()

    exports = parse_exports()
    assert len(exports) == 262
    assert sorted(exports.values()) == list(range(1, 263))
    assert exports["slInit"] == 23

    proxy = PROXY.read_text(encoding="utf-8")
    abi = ATTACH_ABI.read_text(encoding="utf-8")
    installer = INSTALLER.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")

    assert 'kStockInterposerName[] = L"sl_interposer_apusr_stock.dll"' in proxy
    assert 'GetProcAddress(attach, "apusrAttachToStreamlineModule")' in proxy
    assert 'GetProcAddress(stock, "slInit")' in proxy
    assert "stock_semantics=1 loader_shim_only=1" in proxy
    assert "ApusrAttachToStreamlineModuleFn" in abi

    assert 'set(_apusr_streamline_bootstrap_exports' in cmake
    assert 'LIBRARY sl.interposer.dll\\nEXPORTS\\n' in cmake
    assert 'LIBRARY sl_interposer_apusr_stock.dll\\nEXPORTS\\n' in cmake
    assert '=sl_interposer_apusr_stock.${_apusr_streamline_export_name}' in cmake
    assert 'COMMAND "${CMAKE_AR}" /nologo /machine:x64' in cmake
    assert 'target_link_libraries(apusr_streamline_bootstrap_proxy PRIVATE' in cmake
    assert '"${_apusr_streamline_stock_import_lib}"' in cmake

    assert 'move /y "%APUSR_STOCK%" "%APUSR_BACKING%"' in installer
    assert 'fc /b "%APUSR_STOCK%" "%APUSR_PROXY%"' in installer
    assert 'del /f /q "%APUSR_STOCK%"' in installer
    assert "Refusing to overwrite a non-APUSR sl.interposer.dll" in installer
    assert "powershell" not in installer.lower()

    if args.dll is not None:
        entries = load_pe_export_entries(args.dll)
        assert {name: ordinal for name, (ordinal, _) in entries.items()} == exports
        assert entries["slInit"][1] is None
        for name, (_, forwarder) in entries.items():
            if name == "slInit":
                continue
            assert forwarder == f"sl_interposer_apusr_stock.{name}", (name, forwarder)
    for stock in args.stock_dll:
        assert load_pe_exports(stock) == exports, f"stock export surface drift: {stock}"

    print("APUSR Streamline bootstrap proxy contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

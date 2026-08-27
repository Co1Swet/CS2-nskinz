#!/usr/bin/env python3
"""Validate the two portable nSkinz release DLLs and CS2 Chinese catalog data."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


VPK_SIGNATURE = 0x55AA1234
VPK_DIRECTORY_ARCHIVE = 0x7FFF


def read_c_string(stream) -> str:
    data = bytearray()
    while True:
        byte = stream.read(1)
        if not byte:
            raise EOFError("unexpected end of VPK directory tree")
        if byte == b"\0":
            return data.decode("utf-8")
        data.extend(byte)


def archive_path(directory_vpk: Path, archive_index: int) -> Path:
    stem = directory_vpk.stem
    if stem.endswith("_dir"):
        stem = stem[:-4]
    return directory_vpk.with_name(f"{stem}_{archive_index:03d}{directory_vpk.suffix}")


def read_vpk_entry(directory_vpk: Path, requested_path: str) -> bytes:
    with directory_vpk.open("rb") as stream:
        signature, version, tree_size = struct.unpack("<III", stream.read(12))
        if signature != VPK_SIGNATURE:
            raise ValueError("invalid VPK signature")

        header_size = 12
        file_data_section_size = 0
        if version == 2:
            fields = stream.read(16)
            if len(fields) != 16:
                raise EOFError("truncated VPK v2 header")
            file_data_section_size, _, _, _ = struct.unpack("<IIII", fields)
            header_size += 16
        elif version != 1:
            raise ValueError(f"unsupported VPK version {version}")

        tree_end = header_size + tree_size
        match: tuple[int, int, int, int, int] | None = None
        while (extension := read_c_string(stream)):
            while (directory := read_c_string(stream)):
                while (filename := read_c_string(stream)):
                    entry = stream.read(18)
                    if len(entry) != 18:
                        raise EOFError("truncated VPK entry")
                    _, preload_bytes, archive_index, offset, length, terminator = struct.unpack(
                        "<IHHIIH", entry
                    )
                    if terminator != 0xFFFF:
                        raise ValueError("invalid VPK entry terminator")
                    preload_offset = stream.tell()

                    full_path = ""
                    if directory and directory != " ":
                        full_path += f"{directory}/"
                    full_path += filename
                    if extension and extension != " ":
                        full_path += f".{extension}"

                    if full_path == requested_path:
                        match = (
                            preload_bytes,
                            archive_index,
                            offset,
                            length,
                            preload_offset,
                        )
                    stream.seek(preload_bytes, 1)
                    if match is not None:
                        break
                if match is not None:
                    break
            if match is not None:
                break

        if match is None:
            raise FileNotFoundError(f"{requested_path} was not found in {directory_vpk}")

        preload_bytes, archive_index, offset, length, preload_offset = match
        stream.seek(preload_offset)
        result = stream.read(preload_bytes)

    if length:
        payload_path = (
            directory_vpk
            if archive_index == VPK_DIRECTORY_ARCHIVE
            else archive_path(directory_vpk, archive_index)
        )
        payload_offset = (
            tree_end + file_data_section_size + offset
            if archive_index == VPK_DIRECTORY_ARCHIVE
            else offset
        )
        with payload_path.open("rb") as payload:
            payload.seek(payload_offset)
            result += payload.read(length)
    return result


def decode_localization(data: bytes) -> str:
    if data.startswith(b"\xff\xfe"):
        return data[2:].decode("utf-16-le")
    if data.startswith(b"\xfe\xff"):
        return data[2:].decode("utf-16-be")
    return data.removeprefix(b"\xef\xbb\xbf").decode("utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--english-dll", required=True, type=Path)
    parser.add_argument("--chinese-dll", required=True, type=Path)
    parser.add_argument("--vpk", required=True, type=Path)
    args = parser.parse_args()

    english = args.english_dll.read_bytes()
    chinese = args.chinese_dll.read_bytes()
    require(b"resource/csgo_english.txt" in english, "English DLL does not select csgo_english.txt")
    require(b"resource/csgo_schinese.txt" in chinese, "Chinese DLL does not select csgo_schinese.txt")
    ui_samples = ["枪械", "刀", "手套", "挂件", "原皮（无涂装）", "更新", "保存", "加载"]
    missing_ui = [text for text in ui_samples if text.encode("utf-8") not in chinese]
    require(not missing_ui, f"Chinese DLL is missing UI strings: {missing_ui}")

    localization = decode_localization(read_vpk_entry(args.vpk, "resource/csgo_schinese.txt"))
    require("\ufffd" not in localization, "CS2 Chinese localization decoded with replacement characters")
    skin_samples = ["虎牙", "多普勒", "伽玛多普勒", "表面淬火"]
    missing_skins = [text for text in skin_samples if text not in localization]
    require(not missing_skins, f"CS2 Chinese localization is missing skin names: {missing_skins}")

    print("release artifact text validation passed")
    print(f"Chinese UI samples: {', '.join(ui_samples)}")
    print(f"Chinese skin samples: {', '.join(skin_samples)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

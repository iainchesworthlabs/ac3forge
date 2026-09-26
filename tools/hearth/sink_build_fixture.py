"""A hearth_sink build directory made from nothing, for the tests of
package_firmware.py and tools/ci/check_firmware_package.py.

It holds what an ESP-IDF v6.1 build of esp-idf/ac3forge/examples/hearth_sink
leaves that packaging reads: project_description.json, flash_args, the
sdkconfig, the bootloader, the partition table, the empty otadata, the app
image and its ELF, the FAT image, and the audio partition's source outside
the build directory, which flash_args names by a relative path as the real
builds do. The app image is whole: one segment starting with the application
description, the checksum byte that ends its last 16-byte block, and the
SHA-256 the build appends (esp_app_format.h, esp_image_format.c).

Not a test module itself (no test_ prefix), so unittest discovery skips it.
"""

from __future__ import annotations

import hashlib
import json
import struct
from pathlib import Path

CHIP_IDS = {"esp32s3": 0x0009, "esp32c6": 0x000D, "esp32p4": 0x0012}
FLASH_CODES = {"4MB": 2, "16MB": 4}

# The tables O1 gave the boards, as esp-idf/ac3forge/examples/hearth_sink's
# partitions.csv (16 MB) and partitions_c6.csv (4 MB) have them: label, type,
# subtype, offset, size.
TABLE_16MB = [
    ("nvs", 1, 0x02, 0x9000, 0x6000),
    ("phy_init", 1, 0x01, 0xF000, 0x1000),
    ("otadata", 1, 0x00, 0x10000, 0x2000),
    ("ota_0", 0, 0x10, 0x20000, 0x400000),
    ("ota_1", 0, 0x11, 0x420000, 0x400000),
    ("coredump", 1, 0x03, 0x820000, 0x10000),
    ("audio", 1, 0x40, 0x830000, 0x40000),
    ("storage", 1, 0x81, 0x870000, 0x40000),
    ("reserve", 1, 0x41, 0x8B0000, 0x400000),
]
TABLE_4MB = [
    ("nvs", 1, 0x02, 0x9000, 0x6000),
    ("phy_init", 1, 0x01, 0xF000, 0x1000),
    ("otadata", 1, 0x00, 0x10000, 0x2000),
    ("ota_0", 0, 0x10, 0x20000, 0x1C0000),
    ("ota_1", 0, 0x11, 0x1E0000, 0x1C0000),
    ("coredump", 1, 0x03, 0x3A0000, 0x10000),
    ("audio", 1, 0x40, 0x3B0000, 0x10000),
    ("storage", 1, 0x81, 0x3C0000, 0x40000),
]


def app_image(
    chip_id: int,
    version: str,
    *,
    project: str = "ac3forge_hearth_sink",
    min_rev: int = 0,
    max_rev: int = 99,
    flash_code: int = 4,
    segment_bytes: int = 1024,
    seed: int = 1,
) -> bytes:
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = 1  # one segment
    header[2] = 2  # DIO
    header[3] = (flash_code << 4) | 0x0F
    struct.pack_into("<H", header, 12, chip_id)
    header[14] = min_rev // 100
    struct.pack_into("<HH", header, 15, min_rev, max_rev)
    header[23] = 1  # hash_appended
    segment = bytearray((i * 7 + seed) & 0xFF for i in range(segment_bytes))
    segment[:256] = bytes(256)
    struct.pack_into("<I", segment, 0, 0xABCD5432)
    segment[16 : 16 + len(version)] = version.encode()
    segment[48 : 48 + len(project)] = project.encode()
    segment[112:116] = b"v6.1"
    segment[144:176] = bytes((seed + i) & 0xFF for i in range(32))
    checksum = 0xEF
    for byte in segment:
        checksum ^= byte
    image = bytearray(header) + struct.pack("<II", 0x3C000020, segment_bytes) + segment
    padded = (len(image) + 1 + 15) & ~15
    image += bytes(padded - len(image))
    image[-1] = checksum
    image += hashlib.sha256(image).digest()
    return bytes(image)


def partition_table(entries: list[tuple[str, int, int, int, int]]) -> bytes:
    table = bytearray()
    for label, ptype, subtype, offset, size in entries:
        entry = struct.pack("<HBBII", 0x50AA, ptype, subtype, offset, size)
        entry += label.encode().ljust(16, b"\0") + bytes(4)
        table += entry
    return bytes(table + b"\xff" * (0xC00 - len(table)))


def make_build(
    root: Path,
    *,
    target: str = "esp32s3",
    version: str = "v0.11.0",
    flash_size: str = "16MB",
    psram: bool = True,
    wifi_ssid: str = "",
    min_rev: int = 0,
    max_rev: int = 99,
    app: bytes | None = None,
) -> Path:
    """A build directory under `root`, with the audio source beside it; returns the directory."""
    build = root / "build"
    (build / "bootloader").mkdir(parents=True, exist_ok=True)
    (build / "partition_table").mkdir(parents=True, exist_ok=True)
    (root / "stream").mkdir(parents=True, exist_ok=True)
    table = TABLE_16MB if flash_size == "16MB" else TABLE_4MB
    bootloader_at = 0x2000 if target == "esp32p4" else 0x0
    if app is None:
        app = app_image(
            CHIP_IDS[target],
            version,
            flash_code=FLASH_CODES[flash_size],
            min_rev=min_rev,
            max_rev=max_rev,
        )
    (build / "bootloader" / "bootloader.bin").write_bytes(bytes(range(256)) * 20)
    (build / "partition_table" / "partition-table.bin").write_bytes(partition_table(table))
    (build / "ota_data_initial.bin").write_bytes(b"\xff" * 0x2000)
    (build / "ac3forge_hearth_sink.bin").write_bytes(app)
    (build / "ac3forge_hearth_sink.elf").write_bytes(b"\x7fELF" + bytes(2000))
    (build / "storage.bin").write_bytes(b"\xeb\x3c\x90" + bytes(4093))
    (root / "stream" / "sample.ac3").write_bytes(b"\x0b\x77" + bytes(1022))
    audio_at = next(offset for label, _, _, offset, _ in table if label == "audio")
    storage_at = next(offset for label, _, _, offset, _ in table if label == "storage")
    (build / "flash_args").write_text(
        f"--flash-mode dio --flash-freq 80m --flash-size {flash_size}\n"
        f"{bootloader_at:#x} bootloader/bootloader.bin\n"
        "0x8000 partition_table/partition-table.bin\n"
        "0x10000 ota_data_initial.bin\n"
        "0x20000 ac3forge_hearth_sink.bin\n"
        f"{audio_at:#x} ../stream/sample.ac3\n"
        f"{storage_at:#x} storage.bin\n",
        "utf-8",
    )
    (build / "sdkconfig").write_text(
        f'CONFIG_IDF_TARGET="{target}"\n'
        f'CONFIG_ESPTOOLPY_FLASHSIZE="{flash_size}"\n'
        + ("CONFIG_SPIRAM=y\n" if psram else "# CONFIG_SPIRAM is not set\n")
        + f'CONFIG_AC3FORGE_EXAMPLE_WIFI_SSID="{wifi_ssid}"\n'
        'CONFIG_AC3FORGE_EXAMPLE_WIFI_PASSWORD=""\n',
        "utf-8",
    )
    (build / "project_description.json").write_text(
        json.dumps(
            {
                "target": target,
                "app_bin": "ac3forge_hearth_sink.bin",
                "app_elf": "ac3forge_hearth_sink.elf",
                "config_file": str(build / "sdkconfig"),
                "project_name": "ac3forge_hearth_sink",
            }
        ),
        "utf-8",
    )
    return build


# The four images a release publishes, as make_build's arguments.
RELEASE_IMAGES = {
    "hearth-sink-esp32s3": {"target": "esp32s3", "flash_size": "16MB", "psram": True},
    "hearth-sink-esp32c6": {"target": "esp32c6", "flash_size": "4MB", "psram": False},
    "hearth-sink-esp32c6-16mb": {"target": "esp32c6", "flash_size": "16MB", "psram": False},
    "hearth-sink-esp32p4-rev1": {
        "target": "esp32p4",
        "flash_size": "16MB",
        "psram": True,
        "min_rev": 100,
        "max_rev": 199,
    },
}

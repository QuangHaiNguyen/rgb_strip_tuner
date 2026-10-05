#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Generate the fw_meta partition image (SPEC-007 FR-19).

The image is 8 KB: sector 0 holds the metadata record of the firmware image (same layout and
CRC-32 as BuildFwMetaRecord() in components/fw_meta/fw_meta.c, force_bootloader = 0xFFFFFFFF),
sector 1 holds a cleared control record (BuildFwCtrlRecord(record, 0, 0)). Unused bytes are 0xFF
(erased flash).

Usage:
    gen_fw_meta.py --image build/debug/rgb_strip_tuner.bin --output build/debug/fw_meta.bin
    gen_fw_meta.py --version 01.00.00 --image-size 1002496 --sha256 <64 hex> --output out.bin

The first form reads the version from the embedded metadata at file offset 288 and computes
image_sha256 as the esp_partition_get_sha256() digest of ota_0 (SPEC-007 FR-15): for an image with an
appended hash, the SHA-256 of the image without its 32-byte appended hash, checked against that hash. The second form
takes the record fields directly (for the host test of the C serializer, SPEC-007 T-3).
"""
import argparse
import hashlib
import re
import struct
import sys
import zlib

FW_META_MAGIC = 0x57424752   # stored little-endian as the bytes 'RGBW'
FW_CTRL_MAGIC = 0x4C525443
FW_EMBEDDED_META_OFFSET = 288
FW_EMBEDDED_META_LEN = 16
FW_IMAGE_HEAD_LEN = FW_EMBEDDED_META_OFFSET + FW_EMBEDDED_META_LEN
FW_FORCE_BOOTLOADER_CLEAR = 0xFFFFFFFF
FW_META_SECTOR_BYTES = 4096
FW_IMAGE_MAGIC = 0xE9
FW_SHA256_LEN = 32
IMAGE_HASH_APPENDED_OFFSET = 23   # esp_image_header_t.hash_appended
# Exactly MM.mm.pp, matched with fullmatch() so that no trailing newline is accepted (FR-19).
VERSION_PATTERN = re.compile(rb'[0-9]{2}\.[0-9]{2}\.[0-9]{2}')
SHA256_HEX_PATTERN = re.compile(r'[0-9a-fA-F]{64}')


def compute_crc32(data: bytes) -> int:
    """CRC-32 IEEE 802.3 (reflected, 0xEDB88320, init and final XOR 0xFFFFFFFF), as ComputeFwCrc32()."""
    return zlib.crc32(data) & 0xFFFFFFFF


def build_meta_record(version: bytes, image_size: int, sha256: bytes) -> bytes:
    """Serialize fw_meta_record_t (60 bytes), as BuildFwMetaRecord()."""
    body = struct.pack('<I9s3sI32s', FW_META_MAGIC, version + b'\0', b'\0' * 3, image_size, sha256)
    return body + struct.pack('<II', compute_crc32(body), FW_FORCE_BOOTLOADER_CLEAR)


def build_ctrl_record(crash_count: int, boot_attempts: int) -> bytes:
    """Serialize fw_ctrl_record_t (12 bytes), as BuildFwCtrlRecord()."""
    body = struct.pack('<IBB2s', FW_CTRL_MAGIC, crash_count, boot_attempts, b'\0' * 2)
    return body + struct.pack('<I', compute_crc32(body))


def build_partition_image(meta_record: bytes, ctrl_record: bytes) -> bytes:
    """Sector 0: metadata record, sector 1: control record, padded with 0xFF."""
    sector0 = meta_record.ljust(FW_META_SECTOR_BYTES, b'\xff')
    sector1 = ctrl_record.ljust(FW_META_SECTOR_BYTES, b'\xff')
    return sector0 + sector1


def read_image_fields(image: bytes) -> tuple:
    """Return (version, image_size, sha256) of a firmware .bin; exit with an error if it has no valid metadata."""
    if len(image) < FW_IMAGE_HEAD_LEN or image[0] != FW_IMAGE_MAGIC:
        sys.exit('gen_fw_meta: not an ESP-IDF application image')
    magic, version_field = struct.unpack_from('<I9s', image, FW_EMBEDDED_META_OFFSET)
    if magic != FW_META_MAGIC:
        sys.exit('gen_fw_meta: no embedded firmware metadata at offset %d' % FW_EMBEDDED_META_OFFSET)
    if version_field[8] != 0 or not VERSION_PATTERN.fullmatch(version_field[:8]):
        sys.exit('gen_fw_meta: embedded version is not MM.mm.pp')
    if image[IMAGE_HASH_APPENDED_OFFSET] == 1:
        digest = hashlib.sha256(image[:-FW_SHA256_LEN]).digest()
        if digest != image[-FW_SHA256_LEN:]:
            sys.exit('gen_fw_meta: appended SHA-256 does not match the image')
    else:
        digest = hashlib.sha256(image).digest()
    return version_field[:8], len(image), digest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--image', help='firmware .bin to describe')
    parser.add_argument('--version', help='record version MM.mm.pp (with --image-size and --sha256)')
    parser.add_argument('--image-size', type=int, help='record image size in bytes')
    parser.add_argument('--sha256', help='record digest, 64 hex digits')
    parser.add_argument('--output', required=True, help='output partition image (8 KB)')
    args = parser.parse_args()

    if args.image:
        with open(args.image, 'rb') as image_file:
            version, image_size, digest = read_image_fields(image_file.read())
    elif args.version and args.image_size is not None and args.sha256:
        version = args.version.encode('ascii')
        if not VERSION_PATTERN.fullmatch(version):
            sys.exit('gen_fw_meta: --version is not MM.mm.pp')
        image_size = args.image_size
        if not SHA256_HEX_PATTERN.fullmatch(args.sha256):
            sys.exit('gen_fw_meta: --sha256 must be exactly 64 hex digits')
        digest = bytes.fromhex(args.sha256)
    else:
        sys.exit('gen_fw_meta: give --image, or --version, --image-size and --sha256')

    with open(args.output, 'wb') as output_file:
        output_file.write(build_partition_image(build_meta_record(version, image_size, digest),
                                                build_ctrl_record(0, 0)))
    print('fw_meta.bin: version %s, image size %d bytes' % (version.decode('ascii'), image_size))


if __name__ == '__main__':
    main()

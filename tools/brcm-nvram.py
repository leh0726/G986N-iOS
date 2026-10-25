#!/usr/bin/env python3
"""
A Broadcom radio's NVRAM, from the vendor's text file into what the chip reads (as Linux's brcmfmac does,
firmware.c: brcmf_fw_nvram_strip): one key=value after another, each ended by a zero, comments and blank
lines left out, "boardrev=0xff" added when the file has none, a closing zero, zeroes to a multiple of four
bytes, and last a word with the length in words in its low half and the length's complement in its high one.
The kernel puts it at the end of the radio's RAM (bsd/dev/redwood_pcie.c, step 9).

usage: tools/brcm-nvram.py nvram.txt OUT
"""
import struct
import sys


def strip(text):
    entries = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        if not key or not key.replace("_", "").replace("/", "").isalnum():
            continue
        entries.append(f"{key}={value.strip()}")
    if not any(e.startswith("boardrev=") for e in entries):
        entries.append("boardrev=0xff")
    data = b"".join(e.encode("ascii", "replace") + b"\0" for e in entries) + b"\0"
    data += b"\0" * (-len(data) % 4)
    words = len(data) // 4
    return data + struct.pack("<I", ((~words & 0xffff) << 16) | (words & 0xffff)), len(entries)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    with open(sys.argv[1], encoding="latin-1") as f:
        data, entries = strip(f.read())
    with open(sys.argv[2], "wb") as f:
        f.write(data)
    print(f"brcm-nvram.py: {entries} entries, {len(data)} bytes")


if __name__ == "__main__":
    main()

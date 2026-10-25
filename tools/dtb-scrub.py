#!/usr/bin/env python3
"""Take a phone's own identity out of a device tree read off it (/sys/firmware/fdt).

The tree a running Android exposes is the one its boot loader handed the kernel, and that one has the
boot loader's command line in /chosen/bootargs: the phone's serial numbers among it. A tree kept in the
repository (uefi/files/Resources/DTBs, prebuilt/uefi) is scrubbed with this first. Each value is
overwritten in place with zeros of the same length, so the tree keeps its size and layout; the boot
loader writes /chosen afresh at every boot anyway.

usage: tools/dtb-scrub.py FILE.dtb [...]      (rewrites the files; prints what it zeroed)
       tools/dtb-scrub.py --check FILE.dtb    (exit 1 if anything is left to scrub)
"""
import re
import sys

# boot loader arguments that are this phone's alone (Samsung: serial number, AP serial, device id)
KEYS = (b'androidboot.serialno', b'androidboot.ap_serial', b'androidboot.em.did', b'androidboot.imei',
        b'androidboot.wifimacaddr', b'androidboot.btmacaddr')
# ... and properties of the tree itself that hold the same (their value is a string)
PROPS = (b'serial-number',)


def strings_table(blob):
    """The FDT strings block: offset and bytes (property names are references into it)."""
    if blob[:4] != b'\xd0\x0d\xfe\xed':
        raise ValueError('not a flattened device tree')
    off = int.from_bytes(blob[12:16], 'big')
    size = int.from_bytes(blob[32:36], 'big')
    return off, blob[off:off + size]


def scrub(blob):
    """Zero every identifying value; returns the new blob and what was zeroed."""
    out = bytearray(blob)
    done = []
    for key in KEYS:
        for m in re.finditer(re.escape(key) + rb'=([^ \x00]+)', blob):
            if set(m.group(1)) != {ord('0')}:
                out[m.start(1):m.end(1)] = b'0' * (m.end(1) - m.start(1))
                done.append(f'{key.decode()} ({m.end(1) - m.start(1)} characters)')
    # properties by name: walk the structure block for FDT_PROP tokens naming them
    soff, table = strings_table(blob)
    struct_off = int.from_bytes(blob[8:12], 'big')
    struct_size = int.from_bytes(blob[36:40], 'big')
    wanted = {table.find(p + b'\x00') for p in PROPS if table.find(p + b'\x00') >= 0}
    i = struct_off
    end = struct_off + struct_size
    while i < end:
        token = int.from_bytes(blob[i:i + 4], 'big')
        i += 4
        if token == 1:                                  # FDT_BEGIN_NODE: a name, padded to 4
            n = blob.index(b'\x00', i) - i + 1
            i += (n + 3) & ~3
        elif token == 3:                                # FDT_PROP: length, name offset, value
            length = int.from_bytes(blob[i:i + 4], 'big')
            name = int.from_bytes(blob[i + 4:i + 8], 'big')
            value = i + 8
            if name in wanted and length > 1:
                out[value:value + length - 1] = b'0' * (length - 1)
                done.append(f'property {table[name:table.index(b"\x00", name)].decode()} ({length - 1} bytes)')
            i = value + ((length + 3) & ~3)
        elif token == 9:                                # FDT_END
            break
    return bytes(out), done


def main(argv):
    check = argv[:1] == ['--check']
    files = argv[1:] if check else argv
    if not files:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    left = False
    for path in files:
        with open(path, 'rb') as f:
            blob = f.read()
        new, done = scrub(blob)
        if check:
            if done:
                print(f'{path}: not scrubbed: {", ".join(done)}')
                left = True
            continue
        if done:
            with open(path, 'wb') as f:
                f.write(new)
            print(f'{path}: zeroed {", ".join(done)}')
        else:
            print(f'{path}: nothing to scrub')
    return 1 if left else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""Apple DiskCopy 4.2 -> raw sectors + tags, for the M5 disk path.

The System disks on earlymacintosh.org come as StuffIt (.sit) inside a ZIP;
extract with:  unzip <dl>.sit && unar '<name>.sit'  ->  a DiskCopy 4.2 .image.
This tool splits that container into the 400K logical sector data (800 x 512)
and the tag data (800 x 12). Bring-your-own — disk images are gitignored.

  dc42.py "System Disk.image" out_data.bin out_tags.bin
"""
import struct, sys

def main(src, out_data, out_tags):
    d = open(src, "rb").read()
    name = d[1:1 + d[0]].decode("mac-roman", "replace")
    dataSize, tagSize, dataCk, tagCk = struct.unpack_from(">IIII", d, 0x40)
    fmt, fmtByte = d[0x50], d[0x51]
    data = d[84:84 + dataSize]
    tags = d[84 + dataSize:84 + dataSize + tagSize]
    print("name=%r data=%d (%d sectors) tags=%d fmt=0x%02X/0x%02X"
          % (name, dataSize, dataSize // 512, tagSize, fmt, fmtByte))
    boot = struct.unpack_from(">H", data, 0)[0]
    print("boot signature: 0x%04X %s" % (boot, "(bootable 'LK')" if boot == 0x4C4B else "(not bootable)"))
    open(out_data, "wb").write(data)
    open(out_tags, "wb").write(tags)

if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2], sys.argv[3])

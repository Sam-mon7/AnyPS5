import os
import struct
import subprocess
import sys


def convert(converter, filename):
    subprocess.run([converter, "-C", filename], check=True)
    with open(filename, "r+b") as image:
        header = image.read(64)
        if len(header) != 64 or header[:2] != b"MZ":
            raise ValueError("cv2pdb output is not a PE image")
        image.seek(struct.unpack_from("<I", header, 60)[0])
        header = image.read(64)
        if len(header) != 64 or header[:4] != b"PE\0\0":
            raise ValueError("cv2pdb output has no PE header")
        alignment = struct.unpack_from("<I", header, 60)[0]
        if alignment == 0 or alignment & (alignment - 1):
            raise ValueError("cv2pdb output has invalid file alignment")
        size = image.seek(0, os.SEEK_END)
        image.truncate((size + alignment - 1) & -alignment)


if __name__ == "__main__":
    convert(*sys.argv[1:])

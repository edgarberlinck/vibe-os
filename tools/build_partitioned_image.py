#!/usr/bin/env python3
import argparse
import os
import shutil
import struct
import subprocess
import tempfile


def run(cmd):
    subprocess.run(cmd, check=True)


def write_file(path, data, offset=0):
    with open(path, "r+b") as handle:
        handle.seek(offset)
        handle.write(data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True)
    parser.add_argument("--mbr", required=True)
    parser.add_argument("--vbr", required=True)
    parser.add_argument("--stage2", required=True)
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--image-total-sectors", type=int, required=True)
    parser.add_argument("--boot-partition-start-lba", type=int, required=True)
    parser.add_argument("--boot-partition-sectors", type=int, required=True)
    parser.add_argument("--boot-partition-reserved-sectors", type=int, required=True)
    parser.add_argument("--boot-stage2-start-sector", type=int, required=True)
    parser.add_argument("--boot-kernel-start-sector", type=int, required=True)
    args = parser.parse_args()

    image_size = args.image_total_sectors * 512
    with open(args.image, "wb") as image:
        image.truncate(image_size)

    with open(args.mbr, "rb") as handle:
        write_file(args.image, handle.read(), 0)

    with tempfile.TemporaryDirectory() as tmpdir:
        part_path = os.path.join(tmpdir, "bootpart.img")
        with open(part_path, "wb") as part:
            part.truncate(args.boot_partition_sectors * 512)

        run(
            [
                shutil.which("mkfs.fat") or "mkfs.fat",
                "-F",
                "32",
                "-n",
                "VIBEBOOT",
                "-R",
                str(args.boot_partition_reserved_sectors),
                "-h",
                str(args.boot_partition_start_lba),
                part_path,
            ]
        )

        with open(part_path, "r+b") as part, open(args.vbr, "rb") as vbr_handle:
            vbr = bytearray(part.read(512))
            custom = vbr_handle.read(512)
            # Keep the FAT32 BPB/EBPB fields, but replace the VBR entry jump
            # and boot code with our loader.
            vbr[0:3] = custom[0:3]
            vbr[90:510] = custom[90:510]
            vbr[510:512] = custom[510:512]
            part.seek(0)
            part.write(vbr)

        with open(args.stage2, "rb") as stage2_handle:
            stage2 = stage2_handle.read()
        stage2_sectors = (len(stage2) + 511) // 512
        if stage2_sectors + args.boot_stage2_start_sector > args.boot_kernel_start_sector:
            raise SystemExit("stage2 overlaps reserved kernel slot")

        with open(args.kernel, "rb") as kernel_handle:
            kernel = kernel_handle.read()
        kernel_sectors = (len(kernel) + 511) // 512
        if kernel_sectors + args.boot_kernel_start_sector > args.boot_partition_reserved_sectors:
            raise SystemExit("kernel does not fit inside FAT32 reserved sectors")

        with open(part_path, "r+b") as part:
            part.seek(args.boot_stage2_start_sector * 512)
            part.write(stage2)
            stage2_padding = (stage2_sectors * 512) - len(stage2)
            if stage2_padding > 0:
                part.write(b"\0" * stage2_padding)

            part.seek(args.boot_kernel_start_sector * 512)
            part.write(kernel)
            padding = (kernel_sectors * 512) - len(kernel)
            if padding > 0:
                part.write(b"\0" * padding)

        # Keep a visible copy of the kernel inside FAT32 for diagnostics.
        run(
            [
                shutil.which("mcopy") or "mcopy",
                "-i",
                part_path,
                "-o",
                args.kernel,
                "::KERNEL.BIN",
            ]
        )

        with open(part_path, "rb") as part:
            write_file(args.image, part.read(), args.boot_partition_start_lba * 512)


if __name__ == "__main__":
    main()

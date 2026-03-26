#!/usr/bin/env python3
import argparse
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional


@dataclass
class GpuScenario:
    name: str
    description: str
    qemu_args: List[str]
    must_have: List[str]
    wait_markers: Optional[List[str]] = None


@dataclass
class GpuScenarioResult:
    scenario: GpuScenario
    passed: bool
    log: str
    missing_markers: List[str]
    error: Optional[str] = None


class QemuBoot:
    def __init__(self, qemu_binary: str, image_path: Path, memory_mb: int, workspace: Path, extra_args: List[str]):
        self.serial_log = workspace / "serial.log"
        self.proc = subprocess.Popen(
            [
                qemu_binary,
                "-m",
                str(memory_mb),
                "-drive",
                f"format=raw,file={image_path}",
                "-boot",
                "c",
                "-display",
                "none",
                "-serial",
                f"file:{self.serial_log}",
                "-monitor",
                "none",
                "-no-reboot",
                "-no-shutdown",
                *extra_args,
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    def close(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)

    def read_log(self) -> str:
        if not self.serial_log.exists():
            return ""
        return self.serial_log.read_text(encoding="utf-8", errors="replace")

    def wait_for_any(self, markers: List[str], timeout: float) -> None:
        deadline = time.time() + timeout
        while time.time() < deadline:
            log = self.read_log()
            if any(marker in log for marker in markers):
                return
            if self.proc.poll() is not None:
                break
            time.sleep(0.05)
        raise RuntimeError("Timed out waiting for GPU detection markers")


DEFAULT_SCENARIOS = [
    GpuScenario(
        name="std-vga",
        description="Bochs/QEMU VGA should load native_gpu_bga",
        qemu_args=["-vga", "std"],
        must_have=[
            "drm: candidate backend=native_gpu_bga device=bochs_vbe",
            "probe=0",
        ],
    ),
    GpuScenario(
        name="cirrus",
        description="Cirrus should be detected as unsupported native GPU",
        qemu_args=["-vga", "cirrus"],
        must_have=[
            "drm: candidate backend=native_gpu_cirrus device=cirrus_5446",
        ],
    ),
    GpuScenario(
        name="vmware",
        description="VMware SVGA should be detected as unsupported native GPU",
        qemu_args=["-vga", "vmware"],
        must_have=[
            "drm: candidate backend=native_gpu_vmware device=vmware_svga2",
        ],
    ),
    GpuScenario(
        name="qxl",
        description="QXL should be detected as unsupported native GPU",
        qemu_args=["-vga", "qxl"],
        must_have=[
            "drm: candidate backend=native_gpu_qxl device=qxl_vga",
        ],
    ),
    GpuScenario(
        name="virtio-vga",
        description="virtio-vga should be detected as unsupported native GPU",
        qemu_args=["-vga", "none", "-device", "virtio-vga"],
        must_have=[
            "drm: candidate backend=native_gpu_virtio device=virtio_vga",
        ],
    ),
]

RECOVERY_SCENARIOS = [
    GpuScenario(
        name="std-vga-recovery",
        description="Bochs/QEMU VGA should recover through drm revert after a forced native handoff failure",
        qemu_args=["-vga", "std"],
        must_have=[
            "drm: candidate backend=native_gpu_bga device=bochs_vbe",
            "video: drm recovery selftest begin",
            "video: forcing native handoff failure test",
            "video: reverted native modeset after handoff failure",
            "video: drm recovery selftest restored",
        ],
        wait_markers=[
            "video: drm recovery selftest restored",
            "video: drm recovery selftest failed",
        ],
    ),
]


def parse_mode_text(mode_text: str) -> Optional[str]:
    parts = mode_text.lower().split("x")
    if len(parts) != 2:
        return None
    try:
        width = int(parts[0], 10)
        height = int(parts[1], 10)
    except ValueError:
        return None
    if width <= 0 or height <= 0:
        return None
    return f"{width}x{height}"


def run_scenario(qemu_binary: str, image_path: Path, memory_mb: int, scenario: GpuScenario) -> GpuScenarioResult:
    with tempfile.TemporaryDirectory(prefix=f"vibe-gpu-{scenario.name}-") as temp_dir:
        workspace = Path(temp_dir)
        scenario_image = workspace / "boot.img"
        shutil.copyfile(image_path, scenario_image)

        session = QemuBoot(qemu_binary, scenario_image, memory_mb, workspace, scenario.qemu_args)
        error: Optional[str] = None
        log = ""
        try:
            session.wait_for_any(
                scenario.wait_markers or ["drm: candidate backend=", "drm: no native candidate detected"],
                timeout=20.0,
            )
            time.sleep(1.0)
        except Exception as exc:
            error = str(exc)
        finally:
            log = session.read_log()
            session.close()

    missing = [marker for marker in scenario.must_have if marker not in log]
    return GpuScenarioResult(
        scenario=scenario,
        passed=(not missing and error is None),
        log=log,
        missing_markers=missing,
        error=error,
    )


def write_report(report_path: Path, results: List[GpuScenarioResult]) -> None:
    report_path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# GPU Backend Validation Report",
        "",
        "## QEMU GPU Matrix",
        "",
        "| Scenario | Result | Notes |",
        "| --- | --- | --- |",
    ]

    for result in results:
        notes = result.scenario.description
        if result.missing_markers:
            notes = "missing: " + ", ".join(result.missing_markers)
        if result.error:
            notes = (notes + " | " if notes else "") + result.error
        lines.append(f"| {result.scenario.name} | {'PASS' if result.passed else 'FAIL'} | {notes} |")

    lines.extend(["", "## Marker Summary", ""])
    for result in results:
        lines.append(f"### {result.scenario.name}")
        if result.passed:
            lines.append("- required markers observed")
        else:
            lines.append("- missing markers: " + ", ".join(result.missing_markers))
        if result.error:
            lines.append("- error: " + result.error)
        preview = [
            line
            for line in result.log.strip().splitlines()
            if "drm:" in line or "i915:" in line or "bga:" in line or "video:" in line
        ][-20:]
        if preview:
            lines.extend(["", "```text", *preview, "```"])
        lines.append("")

    report_path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate GPU backend detection in headless QEMU.")
    parser.add_argument("--image", required=True, help="boot image to validate")
    parser.add_argument("--report", required=True, help="markdown report output path")
    parser.add_argument("--qemu", default="qemu-system-i386", help="QEMU binary")
    parser.add_argument("--memory-mb", type=int, default=3072, help="guest RAM in MB")
    parser.add_argument("--expect-recovery", action="store_true",
                        help="validate the forced post-modeset recovery path instead of the default detection matrix")
    parser.add_argument("--expect-boot-mode",
                        help="require a boot-time video init marker for the given WxH mode, e.g. 800x600")
    args = parser.parse_args()

    image_path = Path(args.image).resolve()
    report_path = Path(args.report).resolve()
    if not image_path.is_file():
        print(f"error: boot image not found: {image_path}", file=sys.stderr)
        return 1

    qemu_binary = shutil.which(args.qemu) or shutil.which("qemu-system-x86_64")
    if qemu_binary is None:
        print("error: no QEMU binary found", file=sys.stderr)
        return 1

    scenarios = RECOVERY_SCENARIOS if args.expect_recovery else DEFAULT_SCENARIOS
    expected_boot_mode = None
    if args.expect_boot_mode:
        expected_boot_mode = parse_mode_text(args.expect_boot_mode)
        if expected_boot_mode is None:
            print("error: invalid --expect-boot-mode value, expected WxH", file=sys.stderr)
            return 1
        scenarios = [
            GpuScenario(
                name=f"{scenario.name}-boot-{expected_boot_mode}",
                description=f"{scenario.description}; boot should initialize in {expected_boot_mode}",
                qemu_args=scenario.qemu_args,
                must_have=[*scenario.must_have, f"video: boot init backend=", f"mode={expected_boot_mode}x8"],
                wait_markers=[
                    *(scenario.wait_markers or ["drm: candidate backend=", "drm: no native candidate detected"]),
                    "video: boot init backend=",
                ],
            )
            for scenario in scenarios
        ]
    results = [run_scenario(qemu_binary, image_path, args.memory_mb, scenario) for scenario in scenarios]
    write_report(report_path, results)

    failed = [result for result in results if not result.passed]
    if failed:
        print(f"gpu-backends: {len(failed)} scenario(s) failed; see {report_path}", file=sys.stderr)
        return 1

    print(f"gpu-backends: validation passed; report written to {report_path}")
    return 0


if __name__ == "__main__":
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    sys.exit(main())

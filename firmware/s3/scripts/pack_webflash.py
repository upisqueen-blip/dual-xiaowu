#!/usr/bin/env python3
"""构建网页烧录用的合并固件，并按在线刷机页的命名规则输出。

在线刷机页用 esptool-js 把单个文件写到 0x0，所以必须是合并镜像
（bootloader + 分区表 + ota_data + assets + app）；直接传 build/xiaozhi.bin
（应用分区镜像，偏移 0x310000）会导致设备无法启动。

用法：
    python scripts/pack_webflash.py                 # 只打包
    python scripts/pack_webflash.py --publish       # 打包并发布到 GitHub Releases
    python scripts/pack_webflash.py --publish --repo upisqueen-blip/dual-xiaowu

依赖：ESP-IDF 环境已加载（能调用 idf.py 与 esptool）。
"""

from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
MERGED = BUILD / "merged-binary.bin"
SDKCONFIG = ROOT / "sdkconfig"
OUT_DIR = ROOT / "releases"
DEFAULT_REPO = "upisqueen-blip/dual-xiaowu"


def sdkconfig_flags() -> dict[str, bool]:
    flags: dict[str, bool] = {}
    if not SDKCONFIG.exists():
        sys.exit(f"找不到 {SDKCONFIG}，请先在 firmware/s3 下执行过 idf.py build")
    for line in SDKCONFIG.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = line.strip()
        if line.startswith("CONFIG_") and line.endswith("=y"):
            flags[line[:-2]] = True
    return flags


def panel_variant(flags: dict[str, bool]) -> tuple[str, str]:
    """从 sdkconfig 推导 <size>_<colors>，例如 42_3c。"""
    fpc1_583 = flags.get("CONFIG_FPC1_PANEL_583", False)
    fpc2_583 = flags.get("CONFIG_FPC2_PANEL_583", False)
    if fpc1_583 != fpc2_583:
        print(
            "  ⚠ A/B 两屏尺寸不一致（FPC1=%s, FPC2=%s），"
            "在线刷机页按「A/B 同尺寸」匹配，请确认这是有意的"
            % ("5.83" if fpc1_583 else "4.2", "5.83" if fpc2_583 else "4.2"),
            file=sys.stderr,
        )
    size = "583" if fpc1_583 else "42"
    colors = "3c" if flags.get("CONFIG_FPC2_PANEL_COLORS_3", False) else "4c"
    return size, colors


def project_version() -> str:
    for line in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8").splitlines():
        if line.strip().startswith("set(PROJECT_VER"):
            parts = line.split('"')
            if len(parts) >= 2:
                return parts[1]
    return "0.0.0"


def run(cmd: list[str]) -> None:
    print("  $", " ".join(cmd))
    result = subprocess.run(cmd, cwd=str(ROOT))
    if result.returncode != 0:
        sys.exit(f"命令失败（退出码 {result.returncode}）：{' '.join(cmd)}")


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_merged(path: Path) -> None:
    """粗校验：0x0 与 0x310000 必须是镜像魔数 0xE9，0x8000 是分区表 0xAA50。"""
    data = path.read_bytes()
    problems = []
    if data[0] != 0xE9:
        problems.append(f"0x0 魔数=0x{data[0]:02X}（期望 0xE9 bootloader）")
    if int.from_bytes(data[0x8000:0x8002], "little") != 0xAA50:
        problems.append("0x8000 不是分区表（期望 0xAA50）")
    if len(data) <= 0x310000:
        problems.append(f"文件仅 {len(data)} 字节，短于 app 偏移 0x310000")
    elif data[0x310000] != 0xE9:
        problems.append(f"0x310000 魔数=0x{data[0x310000]:02X}（期望 0xE9 app）")
    if problems:
        sys.exit("合并镜像校验失败：\n  - " + "\n  - ".join(problems))


def main() -> None:
    parser = argparse.ArgumentParser(description="打包网页烧录固件")
    parser.add_argument("--publish", action="store_true", help="打包后发布到 GitHub Releases")
    parser.add_argument("--repo", default=DEFAULT_REPO, help=f"publish 目标仓库（默认 {DEFAULT_REPO}）")
    parser.add_argument("--notes", default="", help="Release 说明文件路径（markdown）")
    parser.add_argument("--dry-run", action="store_true", help="只打印将要执行的命令")
    args = parser.parse_args()

    flags = sdkconfig_flags()
    size, colors = panel_variant(flags)
    version = project_version()
    asset_name = f"xiaowu_s3_dual_{size}_{colors}.bin"
    target = OUT_DIR / asset_name

    size_label = "5.83" if size == "583" else "4.2"
    color_label = "三色" if colors == "3c" else "四色"
    print(f"面板配置：{size_label} 英寸 / B 屏 {color_label}（{colors}）")
    print(f"固件版本：v{version}")
    print(f"输出文件：{target}")

    if args.dry_run:
        print("（dry-run，不执行）")
        return

    print("\n[1/4] 合并镜像")
    run(["idf.py", "merge-bin"])
    if not MERGED.exists():
        sys.exit(f"未生成 {MERGED}")

    print("\n[2/4] 校验合并镜像")
    verify_merged(MERGED)

    print("\n[3/4] 输出标准命名")
    OUT_DIR.mkdir(exist_ok=True)
    target.write_bytes(MERGED.read_bytes())
    size_bytes = target.stat().st_size
    digest = sha256_of(target)
    print(f"  {asset_name}  {size_bytes} bytes ({size_bytes / 1048576:.2f} MB)")
    print(f"  sha256 {digest}")

    if not args.publish:
        print("\n完成。上传到 Releases 时资产名必须保持上面的文件名。")
        return

    print(f"\n[4/4] 发布到 {args.repo}")
    tag = f"v{version}-s3-dual-{size}-{colors}"
    cmd = [
        "gh", "release", "create", tag, str(target),
        "-R", args.repo,
        "--title", f"小悟 S3 双屏固件 v{version}（{size_label} 英寸 / B 屏 {color_label}）",
        "--target", "main",
    ]
    if args.notes:
        cmd += ["--notes-file", args.notes]
    else:
        cmd += ["--notes", f"合并镜像，烧录地址 0x0。sha256 {digest}，{size_bytes} 字节。"]
    run(cmd)
    print("\n完成。在线刷机页在下一次固件列表刷新（默认 120 秒缓存）后可见。")


if __name__ == "__main__":
    main()

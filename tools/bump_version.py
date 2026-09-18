#!/usr/bin/env python3
"""版本号升降器 —— 仓库根目录 VERSION 文件的唯一维护入口。

版本号在构建期由 cmake/Version.cmake 解析（优先级：-DYUMEIREN_VERSION > 环境变量
YUMEIREN_VERSION > git 精确标签 > VERSION 文件）。这个脚本负责维护那个「源头」，
并可选地把「改版本 → 提交 → 打标签 → 推送」串成一步。

典型用法
--------
    # 看看现在是什么版本
    python tools/bump_version.py --show

    # 自增（SemVer 惯例：patch 修 bug / minor 加功能 / major 破坏性改动）
    python tools/bump_version.py patch        # 1.0.0 -> 1.0.1
    python tools/bump_version.py minor        # 1.0.0 -> 1.1.0
    python tools/bump_version.py major        # 1.0.0 -> 2.0.0

    # 直接指定（可带预发布后缀）
    python tools/bump_version.py --set 2.0.0
    python tools/bump_version.py --set 1.1.0-rc1

    # 一步发版：改号 → 提交 → 打 vX.Y.Z 标签 → 推送
    # 推上去之后 GitHub Actions 会用这个标签号作为构建版本号，产出与标签完全一致的
    # exe（文件属性里的 ProductVersion 就是它），并把 zip 名带上版本。
    python tools/bump_version.py patch --tag --push

选项
----
    --dry-run       只打印将要发生什么，不落盘、不动 git
    --commit        改完自动 git commit（只提交 VERSION 文件）
    --tag           打注解标签 v<版本>（隐含 --commit）
    --push          推送提交与标签（隐含 --tag）
    -m, --message   自定义提交信息

注意：--tag 打的标签是 **v 前缀**（如 v1.0.1），与 CI 的 `v*` 触发规则对齐。
在标签上构建时版本号就是干净的 1.0.1；不在标签上构建则会带 `+提交数.g短sha` 后缀。
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

# 仓库根目录 = 本脚本所在目录的上一级（tools/ 在根目录下）。
REPO_ROOT = Path(__file__).resolve().parent.parent
VERSION_FILE = REPO_ROOT / "VERSION"

# X.Y.Z，可选 -预发布 / +构建元数据 后缀。
VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)((?:[-+][0-9A-Za-z.\-]+)*)$")

BUMP_PARTS = {"major": 0, "minor": 1, "patch": 2}


def force_utf8() -> None:
    """让中文输出在 Windows 控制台 / Git Bash 下都不炸。

    只在 UTF-8 环境里保证正确显示；老式 GBK 控制台可能显示成乱码，
    但不影响脚本行为（版本号本身是纯 ASCII）。
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


def die(msg: str) -> None:
    print(f"错误：{msg}", file=sys.stderr)
    sys.exit(1)


def run_git(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    proc = subprocess.run(
        ["git", *args],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if check and proc.returncode != 0:
        detail = (proc.stderr or proc.stdout or "").strip()
        die(f"git {' '.join(args)} 失败：{detail}")
    return proc


def read_version() -> str:
    if not VERSION_FILE.exists():
        die(f"找不到 {VERSION_FILE}。请新建该文件并写入一行 X.Y.Z。")
    text = VERSION_FILE.read_text(encoding="utf-8").strip()
    if not text:
        die(f"{VERSION_FILE} 是空的，应写入一行 X.Y.Z。")
    # 只取第一行，容忍文件末尾多出来的空行或注释。
    first = text.splitlines()[0].strip()
    if not VERSION_RE.match(first):
        die(f"{VERSION_FILE} 的内容 '{first}' 不是 X.Y.Z 形式（可带 -rc1 之类的后缀）。")
    return first


def write_version(version: str) -> None:
    VERSION_FILE.write_text(version + "\n", encoding="utf-8")


def bump(current: str, part: str) -> str:
    m = VERSION_RE.match(current)
    if not m:
        die(f"当前版本 '{current}' 解析失败。")
    nums = [int(m.group(1)), int(m.group(2)), int(m.group(3))]
    idx = BUMP_PARTS[part]
    nums[idx] += 1
    # SemVer 惯例：低位清零。2.3.4 的 minor 自增 → 2.4.0（不是 2.4.4）。
    for i in range(idx + 1, 3):
        nums[i] = 0
    return ".".join(str(n) for n in nums)


def git_dirty() -> bool:
    proc = run_git("status", "--porcelain", check=False)
    return bool(proc.stdout.strip())


def git_head_ok() -> bool:
    """是不是一个能用的 git 仓库（有 HEAD）。"""
    return run_git("rev-parse", "--verify", "HEAD", check=False).returncode == 0


def main() -> int:
    force_utf8()

    parser = argparse.ArgumentParser(
        prog="bump_version.py",
        description="维护仓库根目录 VERSION 文件，可选地提交 / 打标签 / 推送。",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "示例：\n"
            "  python tools/bump_version.py --show\n"
            "  python tools/bump_version.py patch\n"
            "  python tools/bump_version.py --set 1.1.0-rc1\n"
            "  python tools/bump_version.py patch --tag --push\n"
        ),
    )
    parser.add_argument(
        "part",
        nargs="?",
        choices=sorted(BUMP_PARTS),
        help="自增哪一位：major / minor / patch",
    )
    parser.add_argument("--set", dest="exact", metavar="X.Y.Z", help="直接指定新版本号")
    parser.add_argument("--show", action="store_true", help="只打印当前版本后退出")
    parser.add_argument("--dry-run", action="store_true", help="只打印，不写文件、不动 git")
    parser.add_argument("--commit", action="store_true", help="改完自动 git commit")
    parser.add_argument("--tag", action="store_true", help="打注解标签 v<版本>（隐含 --commit）")
    parser.add_argument("--push", action="store_true", help="推送提交与标签（隐含 --tag）")
    parser.add_argument("-m", "--message", help="自定义提交信息")
    args = parser.parse_args()

    current = read_version()

    if args.show:
        print(f"当前版本：{current}")
        print(f"来源文件：{VERSION_FILE}")
        if git_head_ok():
            tag = run_git("describe", "--tags", "--exact-match", check=False)
            if tag.returncode == 0 and tag.stdout.strip():
                print(f"HEAD 正处于标签 {tag.stdout.strip()} 上 —— 构建出的版本号就是干净的 {current}")
            else:
                count = run_git("rev-list", "--count", "HEAD", check=False).stdout.strip()
                sha = run_git("rev-parse", "--short", "HEAD", check=False).stdout.strip()
                print(f"HEAD 不在标签上 —— 构建出的版本号会带后缀，形如 {current}+{count}.g{sha}")
        return 0

    if args.exact is not None:
        if not VERSION_RE.match(args.exact):
            die(f"--set 的值 '{args.exact}' 不是 X.Y.Z 形式（可带 -rc1 之类的后缀）。")
        new = args.exact
    elif args.part:
        new = bump(current, args.part)
    else:
        parser.error("需要指定 major / minor / patch 之一，或用 --set X.Y.Z / --show")
        return 2

    if new == current:
        print(f"版本号没变化（仍是 {current}），什么都没做。")
        return 0

    print(f"{current}  ->  {new}")

    if args.dry_run:
        print("（--dry-run：未写入文件、未动 git）")
        return 0

    write_version(new)
    print(f"已写入 {VERSION_FILE}")

    do_tag = args.tag or args.push
    do_commit = args.commit or do_tag

    if not do_commit:
        print("未提交。要一步到位可加 --tag（自动提交并打标签）或 --tag --push。")
        return 0

    if not git_head_ok():
        die("当前不是可用的 git 仓库（或还没有任何提交），无法提交 / 打标签。")

    tag_name = f"v{new}"

    if do_tag:
        existing = run_git("tag", "--list", tag_name, check=False).stdout.strip()
        if existing:
            die(f"标签 {tag_name} 已存在。先删掉它（git tag -d {tag_name}）或换个版本号。")
        if git_dirty():
            # 只提醒不阻断：待发布的版本号已经写进 VERSION，别的文件脏不脏不该拦住发版。
            print("提示：工作区还有未提交的改动，标签只包含本次提交。")

    message = args.message or f"release: 版本号 {current} -> {new}"

    run_git("add", "--", str(VERSION_FILE.relative_to(REPO_ROOT)))
    run_git("commit", "-m", message)
    print(f"已提交：{message}")

    if do_tag:
        run_git("tag", "-a", tag_name, "-m", f"Yumeiren {new}")
        print(f"已打标签：{tag_name}")

    if args.push:
        # --follow-tags 只推注解标签，正好覆盖上面打的这种。
        run_git("push", "--follow-tags")
        print("已推送。GitHub Actions 会用标签号作为构建版本号开始发布。")
    else:
        print(f"未推送。要触发发布：git push --follow-tags（或重跑本脚本并加 --push）")

    return 0


if __name__ == "__main__":
    sys.exit(main())

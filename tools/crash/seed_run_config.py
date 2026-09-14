"""为退出崩溃复现/视频冒烟测试准备确定性的运行目录配置(build/config/.ini)。

为什么不用 PowerShell 直接写 INI：
  - QSettings 的 INI 解析会把值里的 `\\x` 当转义序列吃掉，反斜杠路径必须预先
    写成 `\\\\`；本脚本统一改用正斜杠路径，Windows 全部 API 接受，零转义风险。
  - PowerShell 5.1 的 Set-Content/Get-Content 会按本地代码页重编码，中文值
    (如 image/presetDir) 会被写成乱码。这里固定 UTF-8 无 BOM，与 QSettings
    IniFormat 的写入编码一致。

用法:
  python seed_run_config.py <ini路径> --playlist <路径> [--was-playing true|false]
                           [--set key=value ...]
"""
import argparse
import sys
from pathlib import Path


def norm(p: str) -> str:
    return p.replace("\\", "/")


def parse(text: str):
    """保留节顺序与节内键顺序；返回 [(节名, [(键, 值), ...]), ...]。"""
    sections = []
    current = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith((";", "#")):
            continue
        if line.startswith("[") and line.endswith("]"):
            name = line[1:-1]
            existing = next((s for s in sections if s[0].lower() == name.lower()), None)
            if existing is None:
                sections.append((name, []))
                current = sections[-1]
            else:
                current = existing
            continue
        if "=" not in line or current is None:
            continue
        key, value = line.split("=", 1)
        # 用列表而非元组：set_key 需要就地替换已存在的键值
        current[1].append([key.strip(), value.strip()])
    return sections


def set_key(sections, qkey, value):
    group, _, key = qkey.partition("/")
    target = next((s for s in sections if s[0].lower() == group.lower()), None)
    if target is None:
        target = (group, [])
        sections.append(target)
    for pair in target[1]:
        if pair[0].lower() == key.lower():
            pair[1] = value
            return
    target[1].append((key, value))


def render(sections) -> str:
    out = []
    for name, pairs in sections:
        out.append(f"[{name}]")
        out.extend(f"{k}={v}" for k, v in pairs)
        out.append("")
    return "\r\n".join(out) + "\r\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ini")
    ap.add_argument("--playlist", default="")
    ap.add_argument("--was-playing", default=None, choices=["true", "false"])
    ap.add_argument("--volume", default=None)
    ap.add_argument("--diag", default=None, choices=["true", "false"])
    ap.add_argument("--set", action="append", default=[], metavar="KEY=VALUE")
    args = ap.parse_args()

    path = Path(args.ini)
    sections = parse(path.read_text(encoding="utf-8", errors="replace")) if path.exists() else []

    if args.playlist:
        files = [norm(p) for p in args.playlist.split(";") if p.strip()]
        set_key(sections, "video/playlist", ";".join(files))
    if args.was_playing:
        set_key(sections, "video/wasPlaying", args.was_playing)
    if args.volume:
        set_key(sections, "video/volume", args.volume)
    if args.diag:
        set_key(sections, "video/diag", args.diag)
    for pair in args.set:
        key, _, value = pair.partition("=")
        set_key(sections, key.strip(), norm(value.strip()))

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(render(sections).encode("utf-8"))

    # 回读校验：确认写进去的路径与期望一致(而不是被任何一层转义/编码吃掉)
    check = parse(path.read_text(encoding="utf-8"))
    video = next((s for s in check if s[0].lower() == "video"), None)
    got = dict(video[1]) if video else {}
    print("SEEDED", str(path))
    print("  playlist   =", got.get("playlist", got.get("Playlist", "")))
    print("  wasPlaying =", got.get("wasPlaying", got.get("WasPlaying", "")))
    if args.playlist:
        listed = got.get("playlist", got.get("Playlist", "")).split(";")
        missing = [f for f in listed if f and not Path(f).exists()]
        if missing:
            print("  MISSING FILES:", missing)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

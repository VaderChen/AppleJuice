"""從相容性網站的既有資料同步四語首頁。預設僅顯示差異。"""
import argparse
from collections import Counter
from difflib import unified_diff
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
FILES = {
    "en": ("README.md", "Tested games", "Tools listed separately"),
    "zh": ("README.zh-TW.md", "實測遊戲", "另列工具"),
    "ja": ("README.ja.md", "動作確認済みゲーム", "別掲のツール"),
    "ko": ("README.ko.md", "테스트한 게임", "별도 집계 도구"),
}


def updates():
    page = (ROOT / "docs/tested/index.html").read_text()
    data = json.loads(page.split("const D=", 1)[1].split(";\n", 1)[0])
    games = Counter()
    tools = 0
    for row in data["games"]:
        if row["k"] not in ("game", "tool") or row["l"] not in data["levels"]:
            raise ValueError("相容性記錄的類型或評級未知")
        if row["k"] == "game":
            games[row["l"]] += 1
        else:
            tools += 1
    for lang, (name, title, separate) in FILES.items():
        path = ROOT / name
        old = path.read_text()
        lines = [f'**{title} · {sum(games.values())}** ({data["date"]}) · {separate} · {tools}', ""]
        for level in data["levels"]:
            label = data["labels"][lang][level]
            lines.append(f'- <img src="docs/images/support-levels/{level}.png" width="16" alt=""> {label} · {games[level]}')
        pattern = r"(?<=<!-- compatibility:start -->\n).*?(?=<!-- compatibility:end -->)"
        if len(re.findall(pattern, old, re.S)) != 1:
            raise ValueError(f"{name} 的統計標記不完整")
        new = re.sub(pattern, "\n" + "\n".join(lines) + "\n\n", old, flags=re.S)
        if new != old:
            yield path, old, new


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    changes = list(updates())
    for path, old, new in changes:
        if args.write:
            path.write_text(new)
        else:
            sys.stdout.writelines(unified_diff(old.splitlines(True), new.splitlines(True), fromfile=path.name, tofile=path.name))
    return int(args.check and bool(changes))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, KeyError, IndexError, OSError) as error:
        sys.exit(f"相容性資料同步失敗：{error}")

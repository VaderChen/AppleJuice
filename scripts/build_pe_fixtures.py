#!/usr/bin/env python3
"""從來源建立受控 Windows x64 EXE；產物與 manifest 寫入指定輸出或暫存目錄。"""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests/fixtures/pe"
COMMON = ["-nostdlib", "-Wl,--entry,entry,--subsystem,console,--no-insert-timestamp,--strip-all"]
CFLAGS = ["-Os", "-ffreestanding", "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-fno-stack-protector"]
CONSOLE_CASES = (
    ("hello", 0, "Hello from Windows x64 EXE on ARM64!\n"),
    ("exit37", 37, "Hello from Windows x64 EXE on ARM64!\n"),
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def console_case(name):
    for candidate, code, output in CONSOLE_CASES:
        if candidate == name:
            return dict(file=name + ".exe", source="hello.c", exit_code=code,
                        stdout=output, relocatable=False)
    raise ValueError(f"未知的受控 console fixture：{name}")


def build_console_fixture(directory, name="hello", *, fixtures=FIXTURES, gcc=None):
    """以共用配方重建單一 console PE，不修改固定 fixture 或 manifest。"""
    case = console_case(name)
    compiler = gcc or shutil.which("x86_64-w64-mingw32-gcc")
    if not compiler:
        raise RuntimeError("建立受控 PE 需要 x86_64-w64-mingw32-gcc（MinGW x64 GCC）")
    directory.mkdir(parents=True, exist_ok=True)
    source = fixtures / case["source"]
    source_hash = digest(source)
    defines = [f"-DEXIT_CODE={case['exit_code']}"] if case["exit_code"] else []
    target = directory / case["file"]
    subprocess.run([compiler, *COMMON, *CFLAGS, *defines, str(source), "-o", str(target),
                    "-lkernel32"], check=True, timeout=30)
    if digest(source) != source_hash:
        raise RuntimeError("重建期間受控 PE 的來源已變更")
    case.update(sha256=digest(target), source_sha256=source_hash)
    return target, case


def prepare_console_fixture(directory, name="hello", *, fixtures=FIXTURES, verify_saved=False):
    """預設從來源建置；僅明確要求重現舊產物時核對固定 PE／manifest。"""
    if not verify_saved:
        return build_console_fixture(directory, name, fixtures=fixtures)
    expected = console_case(name)
    source = fixtures / expected["source"]
    if not source.is_file():
        raise RuntimeError(f"缺少受控 PE 的來源：{source}")
    manifest_path = fixtures / "manifest.json"
    saved = None
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        matches = [case for case in manifest["cases"] if case["file"] == expected["file"]]
        if (manifest.get("schema") != 1 or len(matches) != 1 or
                manifest.get("common_flags") != COMMON or manifest.get("c_flags") != CFLAGS):
            raise RuntimeError("受控 PE 的 manifest 與建置配方不符")
        saved = matches[0]
        if (any(saved.get(key) != value for key, value in expected.items()) or
                saved.get("source_sha256") != digest(source)):
            raise RuntimeError("受控 PE 的來源或預期結果與 manifest 不符")
    binary = fixtures / expected["file"]
    if saved is not None and binary.exists():
        if digest(binary) != saved["sha256"]:
            raise RuntimeError("受控 PE 的 SHA-256 與 manifest 不符")
        directory.mkdir(parents=True, exist_ok=True)
        target = directory / expected["file"]
        shutil.copy2(binary, target)
        if digest(target) != saved["sha256"]:
            raise RuntimeError("複製期間受控 PE 已變更")
        return target, dict(saved)
    target, built = build_console_fixture(directory, name, fixtures=fixtures)
    if saved is not None and (built["sha256"] != saved["sha256"] or
                              built["source_sha256"] != saved["source_sha256"]):
        raise RuntimeError("受控 PE 重建與 manifest 不符；請核對工具鏈版本與 flags")
    print(f"已在暫存目錄重建受控 PE：{expected['file']}", flush=True)
    return target, built


def build_fixture_set(directory, *, fixtures=FIXTURES):
    """建立完整 PE 測試集及本次來源快照，完全不讀取工作樹的二進位或 manifest。"""
    if directory.resolve() == fixtures.resolve():
        raise ValueError("PE 測試輸出必須與來源目錄分開")
    gcc, nasm = shutil.which("x86_64-w64-mingw32-gcc"), shutil.which("nasm")
    if not gcc or not nasm:
        raise RuntimeError("建立完整 PE 測試集需要 x86_64-w64-mingw32-gcc 與 nasm")
    directory.mkdir(parents=True, exist_ok=True)
    versions = {}
    for key, command in (("gcc", [gcc, "--version"]), ("ld", [gcc, "-Wl,--version"]), ("nasm", [nasm, "-v"])):
        versions[key] = subprocess.run(command, capture_output=True, text=True, check=True).stdout.splitlines()[0]
    cases = [build_console_fixture(directory, name, fixtures=fixtures, gcc=gcc)[1]
             for name, _, _ in CONSOLE_CASES]
    source = fixtures / "relocation.asm"
    source_hash = digest(source)
    subprocess.run([nasm, "-f", "win64", str(source), "-o", str(directory / "relocation.obj")],
                   check=True, timeout=30)
    target = directory / "relocation.exe"
    subprocess.run([gcc, *COMMON, "-Wl,--image-base,0x140000000", str(directory / "relocation.obj"),
                    "-o", str(target), "-lkernel32"], check=True, timeout=30)
    cases.append(dict(file=target.name, source=source.name, sha256=digest(target), source_sha256=source_hash,
                      exit_code=0, stdout="Relocated PE: sum(1..100) = 5050\n", relocatable=True))
    for case in cases:
        source, snapshot = fixtures / case["source"], directory / case["source"]
        shutil.copyfile(source, snapshot)
        if digest(source) != case["source_sha256"] or digest(snapshot) != case["source_sha256"]:
            raise RuntimeError("建置期間 PE 測試來源已變更")
    manifest = dict(schema=1, description="無 CRT、僅使用三個 kernel32 API 的受控 PE32+ AMD64 fixtures",
                    toolchain=versions, common_flags=COMMON, c_flags=CFLAGS, cases=cases)
    (directory / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--output", type=pathlib.Path, help="產物與本次 manifest 的輸出目錄")
    mode.add_argument("--verify", action="store_true", help="明確核對既有固定二進位與 manifest 的重現性")
    mode.add_argument("--update", action="store_true", help="明確更新來源目錄中的固定二進位與 manifest")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="x64arm-pe-") as temporary:
        out = args.output.resolve() if args.output else pathlib.Path(temporary)
        manifest = build_fixture_set(out)
        for case in manifest["cases"]:
            built, saved = out / case["file"], FIXTURES / case["file"]
            if args.update:
                shutil.copyfile(built, saved)
            elif args.verify and (not saved.is_file() or built.read_bytes() != saved.read_bytes()):
                raise ValueError(f"{saved.name} 缺少或重建與固定檔不同；請核對工具鏈版本與 flags")
        if args.update:
            (FIXTURES / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        elif args.verify:
            saved = json.loads((FIXTURES / "manifest.json").read_text(encoding="utf-8"))
            if saved["cases"] != manifest["cases"]:
                raise ValueError("來源、二進位或預期結果與 manifest 不一致")
        print("通過：3 組 PE32+ AMD64 fixture 建置、來源快照與 SHA-256 紀錄")


if __name__ == "__main__":
    main()

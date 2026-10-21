#!/usr/bin/env python3
"""依建置目錄序列化 Ninja，並以原生 recompact 修復無效的相依紀錄。"""
import argparse
import contextlib
import fcntl
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import uuid

LOCK_FD = 'X64ARM_NINJA_LOCK_FD'
LOCK_TOKEN = 'X64ARM_NINJA_LOCK_TOKEN'


def build_directory(arguments):
    directory = Path.cwd()
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == '--':
            break
        if argument in ('-C', '-f', '-j', '-k', '-l', '-d', '-t', '-w'):
            index += 1
            if index >= len(arguments):
                break  # 引數錯誤由 Ninja 回報。
            if argument == '-C':
                directory = directory / arguments[index]
            if argument == '-t':
                break  # 工具的引數不再是 Ninja 的全域選項。
        elif argument.startswith('-C'):
            directory = directory / argument[2:]
        elif argument.startswith('-t'):
            break
        index += 1
    return directory.resolve()


@contextlib.contextmanager
def directory_lock(directory, allow_tool_reentry=False):
    path = directory / '.x64arm-ninja.lock'
    # Ninja 自動重新執行 CMake 時，內層工具沿用外層持有的鎖，避免自鎖。
    inherited = os.environ.get(LOCK_FD, '')
    reusable = False
    try:
        descriptor = int(inherited)
        info, expected = os.fstat(descriptor), path.stat()
        if allow_tool_reentry and (info.st_dev, info.st_ino) == (expected.st_dev, expected.st_ino):
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            reusable = True
    except (ValueError, OSError):
        pass
    if reusable:
        yield descriptor
        return
    # CMake 的 execute_process 會關閉繼承的 FD；自動重新 configure 的
    # -t restat 因而改用仍存活的外層租約。僅工具呼叫可重入，完整建置
    # 仍須取得 exclusive lock，避免兩個 producer 共用租約同時寫入。
    token = os.environ.get(LOCK_TOKEN, '')
    reusable_token = False
    if allow_tool_reentry and token:
        try:
            owner = json.loads(path.read_text())
            if owner.get('token') == token:
                os.kill(owner['pid'], 0)
                reusable_token = True
        except (OSError, ValueError, KeyError, TypeError):
            pass
    if reusable_token:
        yield None
        return
    with path.open('a') as stream:
        try:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print(f'等待同一目錄的建置完成：{directory}', flush=True)
            fcntl.flock(stream, fcntl.LOCK_EX)
        # metadata 與鎖使用同一 inode；不 unlink／replace 鎖檔。
        stream.seek(0)
        stream.truncate()
        stream.write(json.dumps({'pid': os.getpid(), 'token': uuid.uuid4().hex}))
        stream.flush()
        yield stream.fileno()


def invalid_dependencies(path):
    """依 Ninja v4 格式檢查；不修改紀錄，也不推測未知格式。"""
    try:
        data = path.read_bytes()
    except FileNotFoundError:
        return False
    if data[:16] != b'# ninjadeps\n\x04\x00\x00\x00':
        return False  # 未知／不完整標頭由 Ninja 原生版本處理。
    offset, paths = 16, set()
    while offset < len(data):
        if len(data) - offset < 4:
            return True
        raw_size, = struct.unpack_from('<I', data, offset)
        size = raw_size & 0x7fffffff
        offset += 4
        if size > (1 << 19) - 1 or size % 4 or size > len(data) - offset:
            return True
        record = memoryview(data)[offset:offset + size]
        offset += size
        if raw_size & 0x80000000:
            if size < 12:
                return True
            output, = struct.unpack_from('<I', record)
            if output >= len(paths):
                return True
            if any(item[0] >= len(paths) for item in struct.iter_unpack('<I', record[12:])):
                return True
        else:
            if size <= 4:
                return True
            checksum, = struct.unpack_from('<I', record, size - 4)
            name = bytes(record[:-4]).rstrip(b'\0')
            if not name or name in paths or checksum != (~len(paths) & 0xffffffff):
                return True
            paths.add(name)
    return False


def main(ninja, arguments=None):
    arguments = list(sys.argv[1:] if arguments is None else arguments)
    if arguments in (['--version'], ['-h'], ['--help']):
        return subprocess.call([ninja, *arguments])
    directory = build_directory(arguments)
    if not directory.is_dir():
        return subprocess.call([ninja, *arguments])
    tool = next((arguments[index + 1] if argument == '-t' and index + 1 < len(arguments)
                 else argument[2:] for index, argument in enumerate(arguments)
                 if argument == '-t' or argument.startswith('-t')), '')
    with directory_lock(directory, allow_tool_reentry=(tool == 'restat')) as descriptor:
        environment = dict(os.environ)
        descriptors = () if descriptor is None else (descriptor,)
        if descriptor is not None:
            environment[LOCK_FD] = str(descriptor)
            environment[LOCK_TOKEN] = json.loads((directory / '.x64arm-ninja.lock').read_text())['token']
        # 普通截斷與編號衝突都交由 Ninja 解析有效紀錄。recompact 重新輸出
        # 有效相依圖，移除反覆留在檔尾的錯誤紀錄；不清除物件檔或整份快取。
        if descriptor is not None and invalid_dependencies(directory / '.ninja_deps'):
            print('整理損壞的 Ninja 相依紀錄，保留有效快取與物件檔…', flush=True)
            repaired = subprocess.call([ninja, '-t', 'recompact'], cwd=directory,
                                       env=environment, pass_fds=descriptors)
            if repaired:
                return repaired
            if invalid_dependencies(directory / '.ninja_deps'):
                print('Ninja 相依紀錄仍無效，停止建置以保留現有資料。', file=sys.stderr)
                return 1
        return subprocess.call([ninja, *arguments], env=environment, pass_fds=descriptors)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ninja', required=True, help='實際 Ninja 執行檔')
    options, arguments = parser.parse_known_args()
    raise SystemExit(main(options.ninja, arguments))

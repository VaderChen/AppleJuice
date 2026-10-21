#!/usr/bin/env python3
"""建置 Go + WebView 啟動器，打包原生 macOS app 與 x64arm 引擎。"""
import argparse
import fcntl
import hashlib
import json
import os
from datetime import datetime
import pathlib
import plistlib
import shutil
import subprocess
import tempfile
import uuid
from zoneinfo import ZoneInfo
from bundle_native_v8 import bundle_native_v8
from bundle_cross_architecture import bundle_engine
from build_pe_fixtures import prepare_console_fixture
from engine_validation import from_evidence, from_state

ROOT = pathlib.Path(__file__).resolve().parents[1]


def read_cache(directory):
    path = directory / 'CMakeCache.txt'
    if not path.exists():
        return {}
    return {line.split(':', 1)[0]: line.split('=', 1)[1]
            for line in path.read_text().splitlines()
            if line and not line.startswith(('#', '//')) and ':' in line and '=' in line}


def ninja_recovery_options(cache):
    """失效入口或套件更新後，使用可執行的 Ninja 重新產生共用鎖入口。"""
    if not cache.get('CMAKE_GENERATOR', '').startswith('Ninja'):
        return []
    tools = [cache.get('CMAKE_MAKE_PROGRAM', '')]
    if cache.get('X64ARM_NINJA_EXECUTABLE'):
        tools.append(cache['X64ARM_NINJA_EXECUTABLE'])
    if all(tool and shutil.which(tool) for tool in tools):
        return []
    for tool in (cache.get('X64ARM_NINJA_EXECUTABLE', ''), 'ninja', 'ninja-build'):
        executable = shutil.which(tool) if tool else None
        if executable:
            return [f'-DCMAKE_MAKE_PROGRAM={executable}']
    raise RuntimeError('建置快取的 Ninja 已失效，且 PATH 中找不到可執行的 Ninja')


def build_jobs(requested=None):
    if requested is not None:
        return requested
    override = os.environ.get('CMAKE_BUILD_PARALLEL_LEVEL')
    if override:
        return int(override)
    cores = int(subprocess.check_output(['sysctl', '-n', 'hw.physicalcpu']))
    memory = int(subprocess.check_output(['sysctl', '-n', 'hw.memsize']))
    # 限制預設平行數；大型 C++ 單元每個工作約需 2 GiB。
    return max(1, min(2, cores, memory // (2 * 1024**3)))


def file_digest(path):
    with path.open('rb') as stream:
        digest = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def bundle_inputs(engine_build, identity, signing_prefix, profile):
    files = {engine_build / 'x64arm', ROOT / 'cmake/jit.entitlements.plist'}
    files.add(ROOT / 'tests/fixtures/pe/hello.c')
    files.add(ROOT / 'scripts/build_pe_fixtures.py')
    files.update((ROOT / 'scripts').glob('bundle_*.py'))
    files.add(pathlib.Path(__file__).resolve())
    files.add(ROOT / 'scripts/engine_validation.py')
    files.update(path for path in (ROOT / 'launcher').glob('*.go')
                 if not path.name.endswith('_test.go'))
    # cgo 的原生視窗與橋接同樣屬於封裝來源，避免原生修正沿用舊 app。
    for extension in ('*.m', '*.mm', '*.c', '*.h'):
        files.update((ROOT / 'launcher').glob(extension))
    files.update(ROOT / 'launcher' / name for name in ('go.mod', 'go.sum'))
    for folder in ('web', 'assets'):
        files.update(path for path in (ROOT / 'launcher' / folder).rglob('*')
                     if path.is_file() and not path.name.startswith(('.', '_')))
    for folder in ('launcher/licenses', 'launcher/web/vendor', 'third_party'):
        files.update((ROOT / folder).glob('*LICENSE'))
    kernel_root = pathlib.Path(read_cache(engine_build).get('APPLEJUICE_KERNEL_ROOT', ROOT / 'x64kernel'))
    files.add(kernel_root / 'manifest.json')
    files.update(path for path in (kernel_root / 'share/AppleJuiceKernel/licenses').glob('*') if path.is_file())
    if profile:
        files.add(pathlib.Path(profile).resolve())
    return {'files': {str(path): file_digest(path) for path in sorted(files) if path.is_file()},
            'identity': identity, 'signing_prefix': signing_prefix,
            'profile': str(profile or ''),
            'go_version': subprocess.check_output(['go', 'version'], text=True).strip(),
            'environment': {key: os.environ.get(key, '') for key in (
                'GOOS', 'GOARCH', 'GOFLAGS', 'GOWORK', 'CGO_ENABLED', 'CC', 'CXX',
                'CGO_CFLAGS', 'CGO_CPPFLAGS', 'CGO_CXXFLAGS', 'CGO_LDFLAGS',
                'SDKROOT', 'MACOSX_DEPLOYMENT_TARGET')}}


def bundle_outputs(app):
    return {str(path.relative_to(app)): file_digest(path)
            for path in sorted(app.rglob('*')) if path.is_file()}


def run(*command, cwd=ROOT, env=None):
    subprocess.run(command, cwd=cwd, env=env, check=True)


def build_engine(directory, targets, jobs, source=None):
    """生成檔案或目錄遺失時重新 configure，再重試一次實際建置。"""
    command = ('cmake', '--build', str(directory), '--target', *targets,
               '--parallel', str(jobs))
    try:
        run(*command)
    except subprocess.CalledProcessError:
        print('建置未完成，正在重新產生建置設定與生成檔案後重試…', flush=True)
        options = ninja_recovery_options(read_cache(directory))
        run('cmake', '-S', str(source or ROOT), '-B', str(directory), '-G', 'Ninja',
            *options)
        run(*command)


def validate_engine(engine, prepared_fixture=None):
    """驗證實際封裝引擎可初始化並執行 PE，簽章成功不代表 ABI 正確。"""
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith('X64ARM_') or key in
                   ('X64ARM_WORK_SESSION_ID', 'X64ARM_BUILD_LEASE_FD')}
    environment['X64ARM_SMAP'] = '0'
    # 封裝檢查使用單一編譯 worker。
    environment['X64ARM_JIT_COMPILER_WORKERS'] = '1'
    checks = []
    with tempfile.TemporaryDirectory(prefix='launcher-engine-smoke-') as temporary:
        folder = pathlib.Path(temporary)
        source, case = prepared_fixture or prepare_console_fixture(folder / 'fixture')
        fixture = folder / source.name
        shutil.copy2(source, fixture)
        if file_digest(fixture) != case['sha256']:
            raise RuntimeError('啟動器引擎 Smoke 的 PE 與驗證清單不符')
        for backend in ('interpreter', 'jit'):
            diagnostics = folder / (backend + '.log')
            command = [str(engine), 'run-pe', str(fixture), '--backend', backend,
                       '--gui', 'headless', '--clock', 'virtual', '--memory-limit-mib', '64',
                       '--max-instructions', '1000000', '--unresolved-imports', 'reject',
                       '--fs-root', str(folder), '--c-drive', str(folder),
                       '--diagnostics-file', str(diagnostics)]
            try:
                result = subprocess.run(command, env=environment, capture_output=True, timeout=30)
            except subprocess.TimeoutExpired as error:
                raise RuntimeError(f'封裝引擎 {backend} PE Smoke 逾時，保留既有 Launcher') from error
            detail = diagnostics.read_text(errors='replace') if diagnostics.exists() else ''
            if (result.returncode != case['exit_code'] or
                    result.stdout != case['stdout'].encode() or
                    not any(line.startswith('reason=exited ') for line in detail.splitlines())):
                raise RuntimeError(
                    f'封裝引擎 {backend} PE Smoke 失敗（退出碼 {result.returncode}），保留既有 Launcher\n'
                    + result.stderr.decode(errors='replace')[-2000:] + '\n' + detail[-2000:])
            checks.append(backend)
    print('封裝引擎 PE Smoke 通過：直譯器、JIT', flush=True)
    return {'schema': 1, 'engine_sha256': file_digest(engine), 'backends': checks,
            'fixture_sha256': case['sha256']}


def remove_appledouble(directory):
    """清除外接磁碟產生的中繼資料，避免 codesign 當成未簽署元件。"""
    for path in directory.rglob('._*'):
        if path.is_file():
            with path.open('rb') as source:
                magic = source.read(4)
            if magic == b'\x00\x05\x16\x07':
                path.unlink()


def handle_cleanup_error(function, path, error):
    # macOS 刪除主檔時可能同時移除 ._ 檔，Python 3.9 的 rmtree 仍會嘗試刪除它。
    if isinstance(error[1], FileNotFoundError) and pathlib.Path(path).name.startswith('._'):
        return
    raise error[1]


def build_launcher():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--skip-engine', action='store_true', help='沿用已建置的 Release 引擎')
    parser.add_argument('--engine-evidence', type=pathlib.Path, help='沿用同一引擎的既有直譯／JIT 驗收，僅重建前端封裝')
    parser.add_argument('--sign-identity', help='固定 macOS 簽署憑證名稱或 SHA-1；未指定時沿用引擎 CMake 設定')
    parser.add_argument('--cross-arch-profile', type=pathlib.Path, help='TSO／4 KiB 授權 profile；未指定時沿用引擎 CMake 設定')
    parser.add_argument('--engine-build-dir', type=pathlib.Path, default=ROOT / 'build' / 'release', help='引擎建置目錄，可指定隔離的 Release 建置')
    parser.add_argument('--refresh-assets', action='store_true', help='從鎖定版本重建 xterm.js 資源')
    parser.add_argument('--jobs', type=int, help='C++／Go 建置平行數；預設最多 2，並依核心及記憶體縮減')
    parser.add_argument('--output', type=pathlib.Path, help='指定 app 輸出路徑，供隔離建置使用')
    parser.add_argument('--force-package', action='store_true', help='強制重新打包及驗證簽章')
    parser.add_argument('--with-native-v8', action='store_true', help='內建 ARM64 V8 模組、相依函式庫與 x86／x64 測試 PE')
    parser.add_argument('--v8-node-root', type=pathlib.Path, default=pathlib.Path('/opt/homebrew/opt/node'), help='V8 標頭及 libnode 的相同安裝根目錄')
    args = parser.parse_args()
    if args.engine_evidence and (not args.skip_engine or args.with_native_v8):
        parser.error('--engine-evidence 需要 --skip-engine，且不能重新封裝 V8 引擎')
    try:
        jobs = build_jobs(args.jobs)
    except ValueError:
        parser.error('CMAKE_BUILD_PARALLEL_LEVEL 必須為正整數')
    if jobs < 1:
        parser.error('--jobs 或 CMAKE_BUILD_PARALLEL_LEVEL 必須大於零')
    engine_build = args.engine_build_dir.resolve()
    cache = read_cache(engine_build)
    if not args.skip_engine:
        # 外部 Make 不共享 Ninja 的工作預算，單一套件只能占用一個上層工作。
        requested = {'X64ARM_EXTERNAL_BUILD_JOBS': '1'}
        if args.sign_identity is not None:
            requested['X64ARM_CODESIGN_IDENTITY'] = args.sign_identity
        if args.cross_arch_profile is not None:
            requested['X64ARM_CROSS_ARCH_PROFILE'] = str(args.cross_arch_profile.resolve())
        if args.with_native_v8:
            requested.update(X64ARM_NATIVE_V8_PROTOTYPE='ON',
                             X64ARM_V8_NODE_ROOT=str(args.v8_node_root.resolve()))
        preset = ROOT / 'CMakePresets.json'
        preset_state = engine_build / '.launcher-preset.sha256'
        preset_hash = file_digest(preset)
        tool_recovery = ninja_recovery_options(cache)
        configure = (not cache or not all((engine_build / name).is_file() for name in
                                         ('build.ninja', 'CMakeFiles/rules.ninja'))
                     or cache.get('CMAKE_BUILD_TYPE') != 'Release'
                     or any(cache.get(key) != value for key, value in requested.items())
                     or not preset_state.exists() or preset_state.read_text() != preset_hash
                     or bool(tool_recovery))
        if tool_recovery:
            print('建置工具快取已失效，正在重新產生 Ninja 共用鎖入口…', flush=True)
        if configure and engine_build == ROOT / 'build' / 'release' and not args.with_native_v8:
            run('cmake', '--preset', 'release', *tool_recovery)
        elif configure:
            run('cmake', '-S', str(ROOT), '-B', str(engine_build), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=OFF', *tool_recovery, *(['-DX64ARM_NATIVE_V8_PROTOTYPE=ON', '-DX64ARM_V8_NODE_ROOT='+str(args.v8_node_root.resolve())] if args.with_native_v8 else []))
        if configure:
            if requested:
                run('cmake', '-S', str(ROOT), '-B', str(engine_build),
                    *(f'-D{key}={value}' for key, value in requested.items()))
            preset_state.write_text(preset_hash)
        print(f'引擎增量建置：{jobs} 個宿主工作', flush=True)
        # Ninja／Make 自動追蹤 CMake 與來源；不在每次啟動時強制重新 configure。
        build_engine(engine_build, ['x64arm', *(['x64arm_v8_host'] if args.with_native_v8 else [])], jobs)
    cache = read_cache(engine_build)
    identity = args.sign_identity if args.sign_identity is not None else cache.get('X64ARM_CODESIGN_IDENTITY', '-')
    signing_prefix = cache.get('X64ARM_CODESIGN_IDENTIFIER_PREFIX', 'org.macwin')
    profile = args.cross_arch_profile or cache.get('X64ARM_CROSS_ARCH_PROFILE')
    evidence = None
    if args.engine_evidence:
        if not profile:
            raise RuntimeError('沿用既有驗收需要已簽署的引擎 app bundle')
        evidence_path = args.engine_evidence.resolve(strict=True)
        evidence_hash = file_digest(evidence_path)
        evidence = json.loads(evidence_path.read_text())
        from_evidence(evidence)
        engine_app = engine_build / 'x64arm.app'
        if file_digest(engine_app / 'Contents/MacOS/x64arm') != evidence['engine_sha256']:
            raise RuntimeError('現有引擎與驗收身分不同，保留既有 Launcher')
        if (file_digest(engine_app / 'Contents/embedded.provisionprofile') != file_digest(pathlib.Path(profile).resolve()) or
                plistlib.loads((engine_app / 'Contents/Info.plist').read_bytes())['CFBundleIdentifier'] != signing_prefix+'.x64arm'):
            raise RuntimeError('已驗證引擎的封裝授權與目前設定不同，保留既有 Launcher')
        for name, digest in evidence['files'].items():
            if file_digest(ROOT / name) != digest:
                raise RuntimeError('驗收來源或證據已變更：' + name)
        engine_contents = bundle_outputs(engine_app)
    if args.refresh_assets:
        run('npm', 'ci', '--ignore-scripts', cwd=ROOT / 'launcher')
        run('npm', 'run', 'vendor', cwd=ROOT / 'launcher')
    destination = (args.output or ROOT / 'build' / 'launcher' / 'Win64 Launcher.app').resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    state_path = destination.with_name('.' + destination.name + '.build-state.json')
    inputs = bundle_inputs(engine_build, identity, signing_prefix, profile)
    # V8 包含外部 dylib 閉包及重定位，保留完整打包／簽章流程。
    if not args.with_native_v8 and not args.force_package and destination.is_dir():
        try:
            state = json.loads(state_path.read_text())
            validation = from_state(state, file_digest(destination/'Contents/MacOS/x64arm-engine'))
            if (state['inputs'] == inputs and state['outputs'] == bundle_outputs(destination)
                    and (not evidence or
                         (validation['engine_sha256'] == evidence['engine_sha256'] and
                          file_digest(destination/'Contents/MacOS/x64arm-engine') == evidence['engine_sha256']))):
                print(f'無須重新打包，沿用已驗證產物：{destination}')
                return
        except (OSError, ValueError, KeyError):
            pass
    # 所有版本欄位使用同一次建置時間，跨午夜也保持一致。
    built_at = datetime.now(ZoneInfo('Asia/Taipei'))
    release_version = built_at.strftime('0.%y.%m%d')
    display_version = f'{release_version} build {built_at:%H%M}'
    # 在全新目錄建置，避免覆寫已載入的 Mach-O，亦避免唯讀 LICENSE 阻止重建。
    with tempfile.TemporaryDirectory(prefix='.launcher-build-', dir=destination.parent) as staging:
        # 先準備測試輸入，缺少工具或完整性不符時不浪費 Go 建置與簽章。
        smoke_fixture = None if evidence else prepare_console_fixture(pathlib.Path(staging) / 'engine-smoke')
        app = pathlib.Path(staging) / destination.name
        macos = app / 'Contents' / 'MacOS'
        macos.mkdir(parents=True, exist_ok=True)
        go_environment = dict(os.environ, GOMAXPROCS=str(jobs))
        run('go', 'build', '-p', str(jobs), '-trimpath', '-ldflags', f'-X "main.launcherVersion={display_version}"', '-o', str(macos / 'Win64Launcher'), '.', cwd=ROOT / 'launcher', env=go_environment)
        if evidence:
            target = app / 'Contents/Helpers/x64arm.app'
            shutil.copytree(engine_app, target, symlinks=True)
            if bundle_outputs(target) != engine_contents:
                raise RuntimeError('已驗證引擎 bundle 複製不完整')
            (macos / 'x64arm-engine').symlink_to('../Helpers/x64arm.app/Contents/MacOS/x64arm')
        elif profile:
            bundle_engine(engine_build / 'x64arm', app / 'Contents/Helpers/x64arm.app',
                          pathlib.Path(profile), identity, signing_prefix+'.x64arm')
            # 保留既有命令列入口與 V8 資源的相對位置；實體可執行檔屬於內層 bundle。
            (macos / 'x64arm-engine').symlink_to('../Helpers/x64arm.app/Contents/MacOS/x64arm')
        else:
            shutil.copy2(engine_build / 'x64arm', macos / 'x64arm-engine')
        licenses = app / 'Contents' / 'Resources' / 'Licenses'
        licenses.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / 'launcher' / 'assets' / 'AppleJuice.icns',
                     licenses.parent / 'AppleJuice.icns')
        for source in [ROOT / 'launcher' / 'licenses', ROOT / 'launcher' / 'web' / 'vendor', ROOT / 'third_party']:
            for license_file in source.glob('*LICENSE'):
                if license_file.name.startswith('._'):
                    continue
                shutil.copy2(license_file, licenses / license_file.name)
        kernel_root = pathlib.Path(cache.get('APPLEJUICE_KERNEL_ROOT', ROOT / 'x64kernel'))
        kernel_licenses = kernel_root / 'share/AppleJuiceKernel/licenses'
        if not (kernel_licenses / 'LICENSE.md').is_file():
            raise RuntimeError('缺少靜態連結核心的授權文件，保留既有 Launcher')
        shutil.copytree(kernel_licenses, licenses / 'AppleJuiceKernel')
        minimum_system = '11.0'
        if args.with_native_v8:
            minimum_system = bundle_native_v8(ROOT, app, engine_build / 'x64arm_v8_host')
        info = {
            'CFBundleName': 'Win64 Launcher', 'CFBundleDisplayName': 'Win64 Launcher',
            'CFBundleIdentifier': 'dev.x64arm.launcher', 'CFBundleExecutable': 'Win64Launcher',
            'CFBundleIconFile': 'AppleJuice.icns',
            'CFBundlePackageType': 'APPL', 'CFBundleVersion': str(built_at.hour * 100 + built_at.minute),
            'CFBundleShortVersionString': release_version, 'Win64LauncherVersion': display_version,
            'LSMinimumSystemVersion': minimum_system, 'NSHighResolutionCapable': True,
            'NSPrincipalClass': 'NSApplication',
        }
        with (app / 'Contents' / 'Info.plist').open('wb') as f:
            plistlib.dump(info, f)
        if not profile:
            run('codesign', '--force', '--sign', identity, '--options', 'runtime', '--identifier', signing_prefix+'.x64arm', '--entitlements',
                str(ROOT / 'cmake' / 'jit.entitlements.plist'), str(macos / 'x64arm-engine'))
        remove_appledouble(app)
        run('codesign', '--force', '--sign', identity, '--options', 'runtime', str(app))
        run('codesign', '--verify', '--deep', '--strict', str(app))
        if evidence:
            if (file_digest(macos / 'x64arm-engine') != evidence['engine_sha256'] or
                    file_digest(evidence_path) != evidence_hash or bundle_outputs(engine_app) != engine_contents or
                    any(file_digest(ROOT / name) != digest for name, digest in evidence['files'].items())):
                raise RuntimeError('前端封裝期間已驗證引擎或驗收來源變更，保留既有 Launcher')
            engine_smoke = from_evidence(evidence)
            print('沿用已驗證引擎及原驗收種類：' + evidence['engine_sha256'], flush=True)
        else:
            engine_smoke = validate_engine(macos / 'x64arm-engine', smoke_fixture)
        if bundle_inputs(engine_build, identity, signing_prefix, profile) != inputs:
            raise RuntimeError('封裝期間來源或引擎已變更，保留既有 Launcher，請完成建置後重試')
        previous = destination.with_name('.launcher-previous-' + uuid.uuid4().hex)
        if destination.exists():
            destination.rename(previous)
        try:
            app.rename(destination)
        except BaseException:
            if previous.exists():
                previous.rename(destination)
            raise
        if previous.exists():
            shutil.rmtree(previous, onerror=handle_cleanup_error)
    state = {'inputs': inputs if not args.with_native_v8 else None,
             'outputs': bundle_outputs(destination), 'engine_smoke': engine_smoke}
    if engine_smoke.get('schema') in (2, 3):
        state['engine_validation'] = state.pop('engine_smoke')
    if evidence:
        state['engine_evidence'] = dict(path=str(evidence_path.relative_to(ROOT)), sha256=evidence_hash)
    temporary_state = state_path.with_name(state_path.name + '.' + uuid.uuid4().hex)
    temporary_state.write_text(json.dumps(state, ensure_ascii=False, sort_keys=True))
    temporary_state.replace(state_path)
    print(f'已建立：{destination}（{display_version}）')


def main():
    # Finder 重複雙擊與終端呼叫共用鎖，避免 Ninja／正式 app 被同時更新。
    lock = ROOT / 'build' / '.launcher-build.lock'
    lock.parent.mkdir(parents=True, exist_ok=True)
    with lock.open('a') as stream:
        fcntl.flock(stream, fcntl.LOCK_EX)
        build_launcher()


if __name__ == '__main__':
    main()

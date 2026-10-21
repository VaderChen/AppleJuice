#!/usr/bin/env python3
"""建立具有獨立 provisioning profile 的跨架構引擎 bundle。"""
import argparse
import pathlib
import plistlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def bundle_engine(binary, output, profile, identity, identifier):
    if identity == '-':
        raise ValueError('跨架構授權需要 profile 對應的 Developer ID 簽署憑證')
    decoded = plistlib.loads(subprocess.check_output(['security', 'cms', '-D', '-i', str(profile)]))
    allowed = decoded['Entitlements']
    application = allowed.get('com.apple.application-identifier', '')
    team = allowed.get('com.apple.developer.team-identifier', '')
    if not team or application != team + '.' + identifier or allowed.get('com.apple.developer.cross-architecture-support') is not True:
        raise ValueError('profile 未授權指定的引擎識別碼與跨架構能力')
    entitlements = plistlib.loads((ROOT / 'cmake/jit.entitlements.plist').read_bytes())
    for key in ('com.apple.application-identifier', 'com.apple.developer.team-identifier', 'com.apple.developer.cross-architecture-support'):
        entitlements[key] = allowed[key]
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.engine-bundle-', dir=output.parent) as temporary:
        app = pathlib.Path(temporary) / output.name
        contents = app / 'Contents'
        (contents / 'MacOS').mkdir(parents=True)
        shutil.copy2(binary, contents / 'MacOS/x64arm')
        shutil.copy2(profile, contents / 'embedded.provisionprofile')
        (contents / 'Info.plist').write_bytes(plistlib.dumps({
            'CFBundleName': 'x64arm', 'CFBundleIdentifier': identifier,
            'CFBundleExecutable': 'x64arm', 'CFBundlePackageType': 'APPL',
            'CFBundleVersion': '1', 'LSBackgroundOnly': True,
        }))
        entitlement_path = pathlib.Path(temporary) / 'entitlements.plist'
        entitlement_path.write_bytes(plistlib.dumps(entitlements))
        for path in app.rglob('._*'):
            if path.is_file() and path.read_bytes()[:4] == b'\x00\x05\x16\x07':
                path.unlink()
        subprocess.run(['codesign', '--force', '--sign', identity, '--options', 'runtime', '--timestamp=none',
                        '--identifier', identifier, '--entitlements', str(entitlement_path), str(app)], check=True)
        subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app)], check=True)
        if output.exists():
            shutil.rmtree(output)
        app.rename(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('binary', 'output', 'profile'):
        parser.add_argument('--' + option, type=pathlib.Path, required=True)
    parser.add_argument('--identity', required=True)
    parser.add_argument('--identifier', default='org.macwin.x64arm')
    args = parser.parse_args()
    bundle_engine(args.binary, args.output, args.profile, args.identity, args.identifier)

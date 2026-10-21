#!/usr/bin/env python3
"""將原生 V8 helper、非系統 dylib 閉包與真 x86／x64 PE 測試打包到 macOS app。"""
import hashlib
import json
import pathlib
import shutil
import stat
import subprocess

SYSTEM_PREFIXES=('/usr/lib/', '/System/Library/')

def output(*args):
    return subprocess.check_output(args,text=True)

def dependencies(path):
    return [line.strip().split(' (compatibility version',1)[0] for line in output('otool','-L',str(path)).splitlines()[1:]]

def rpaths(path,host):
    lines=output('otool','-l',str(path)).splitlines();result=[]
    for at,line in enumerate(lines):
        if line.strip()=='cmd LC_RPATH':
            value=lines[at+2].strip().split('path ',1)[1].split(' (offset',1)[0]
            result.append(pathlib.Path(value.replace('@loader_path',str(path.parent)).replace('@executable_path',str(host.parent))))
    return result

def resolve(name,owner,host):
    if name.startswith('@loader_path/'):
        return (owner.parent/name.removeprefix('@loader_path/')).resolve()
    if name.startswith('@executable_path/'):
        return (host.parent/name.removeprefix('@executable_path/')).resolve()
    if name.startswith('@rpath/'):
        for folder in [*rpaths(owner,host),*rpaths(host,host)]:
            candidate=folder/name.removeprefix('@rpath/')
            if candidate.is_file():return candidate.resolve()
        raise RuntimeError('無法解析 V8 相依函式庫：'+name)
    return pathlib.Path(name).resolve()

def minimum_macos(path):
    lines=output('otool','-l',str(path)).splitlines()
    for index,line in enumerate(lines):
        if line.strip()=='cmd LC_BUILD_VERSION':
            for item in lines[index+1:index+8]:
                if item.strip().startswith('minos '):return item.strip().split()[1]
        if line.strip()=='cmd LC_VERSION_MIN_MACOSX':
            for item in lines[index+1:index+5]:
                if item.strip().startswith('version '):return item.strip().split()[1]
    raise RuntimeError('無法取得 V8 元件的最低 macOS 版本：'+str(path))

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def bundle_native_v8(root,app,host):
    host=host.resolve();assert host.is_file(), '缺少 V8 helper，請先啟用原型建置'
    assert 'arm64' in output('file',str(host)), 'V8 helper 必須為 ARM64'
    binaries={host.name:host};edges={};queue=[host]
    while queue:
        owner=queue.pop();deps=[]
        for name in dependencies(owner):
            if name.startswith(SYSTEM_PREFIXES):continue
            path=resolve(name,owner,host)
            if path==owner:continue
            if not path.is_file():raise RuntimeError('V8 相依函式庫不存在：'+str(path))
            if path.name in binaries and binaries[path.name]!=path:raise RuntimeError('V8 dylib 名稱衝突：'+path.name)
            deps.append((name,path.name))
            if path.name not in binaries:binaries[path.name]=path;queue.append(path)
        edges[owner]=deps
    folder=app/'Contents/Frameworks/V8';folder.mkdir(parents=True,exist_ok=True)
    licenses=app/'Contents/Resources/Licenses/V8';licenses.mkdir(parents=True,exist_ok=True)
    destination=app/'Contents/MacOS/x64arm-v8-host'
    records=[];seen_packages=set()
    for source in binaries.values():
        target=destination if source==host else folder/source.name
        shutil.copy2(source,target);target.chmod(target.stat().st_mode|stat.S_IWUSR)
        if source!=host:subprocess.run(['install_name_tool','-id','@rpath/'+target.name,str(target)],check=True)
        for old,name in edges[source]:
            new=('@loader_path/../Frameworks/V8/' if source==host else '@loader_path/')+name
            subprocess.run(['install_name_tool','-change',old,new,str(target)],check=True)
        records.append({'name':target.name,'source_sha256':sha(source),'minimum_macos':minimum_macos(source),'bundled_path':str(target.relative_to(app))})
        # Homebrew 安裝根目錄的授權文件隨 dylib 一起保存，Node LICENSE 亦含 V8 授權。
        if 'Cellar' in source.parts:
            index=source.parts.index('Cellar');package=pathlib.Path(*source.parts[:index+3])
            if package not in seen_packages:
                seen_packages.add(package);destination_licenses=licenses/package.parent.name;destination_licenses.mkdir(exist_ok=True)
                for license_file in list(package.glob('*LICENSE*'))+list(package.glob('*COPYING*')):
                    if license_file.is_file() and not license_file.name.startswith('._'):shutil.copy2(license_file,destination_licenses/license_file.name)
    # 先簽相依 dylib，再簽 helper；主 app 簽章由既有建置器處理。
    for library in sorted(folder.glob('*.dylib')):subprocess.run(['codesign','--force','--sign','-',str(library)],check=True)
    subprocess.run(['codesign','--force','--sign','-','--options','runtime','--entitlements',str(root/'cmake/native-v8.entitlements.plist'),str(destination)],check=True)
    for binary in [destination,*folder.glob('*.dylib')]:
        for dependency in dependencies(binary):
            assert dependency.startswith(SYSTEM_PREFIXES+('@loader_path/','@rpath/')), (binary,dependency)
    tests=app/'Contents/Resources/V8Tests';tests.mkdir(parents=True,exist_ok=True)
    for arch,prefix,entry in [('x86','i686','_entry'),('x64','x86_64','entry')]:
        compiler=shutil.which(prefix+'-w64-mingw32-gcc');dlltool=shutil.which(prefix+'-w64-mingw32-dlltool');assert compiler and dlltool, 'V8 測試 PE 需要 MinGW'
        definition=tests/(arch+'.def');definition.write_text('LIBRARY x64arm_v8.dll\nEXPORTS\nX64ArmV8Dispatch\n')
        library=tests/(arch+'.a');subprocess.run([dlltool,'-d',str(definition),'-l',str(library)],check=True)
        subprocess.run([compiler,'-O2','-Wall','-Wextra','-Werror','-ffreestanding','-fno-builtin','-fno-stack-protector','-nostdlib','-I',str(root/'include'),'-Wl,--entry,'+entry+',--subsystem,console,--image-base,0x400000',str(root/'tests/fixtures/process/native_v8.c'),str(library),'-lkernel32','-lgcc','-o',str(tests/('native-v8-'+arch+'.exe'))],check=True)
        definition.unlink();library.unlink()
    minimum=max((record['minimum_macos'] for record in records),key=lambda value:tuple(int(part) for part in value.split('.')))
    (app/'Contents/Resources/native-v8-manifest.json').write_text(json.dumps({'schema':1,'module':'x64arm_v8.dll','protocol':1,'minimum_macos':minimum,'binaries':records,'scope':'ARM64 V8 模組原型；尚未透明替換 NW.js／遊戲內嵌 V8'},ensure_ascii=False,indent=2)+'\n')
    return minimum

#!/usr/bin/env python3
"""Download prebuilt CEF and compile only Pragtical's small bridge and host."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parent.parent

def host_arch():
    cpu = {"AMD64":"x86_64", "arm64":"aarch64"}.get(platform.machine(), platform.machine())
    system = {"Linux":"linux", "Darwin":"darwin", "Windows":"windows"}[platform.system()]
    return cpu + "-" + system

def cef_sdk(arch):
    spec = json.loads((ROOT / "cef.json").read_text())["platforms"][arch]
    downloads = ROOT / ".build/downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    archive = downloads / spec["filename"]
    if not archive.exists():
        partial = archive.with_suffix(".partial")
        print("Downloading prebuilt CEF:", spec["url"], flush=True)
        with urllib.request.urlopen(spec["url"], timeout=120) as source, partial.open("wb") as output:
            shutil.copyfileobj(source, output, 1024 * 1024)
        partial.replace(archive)
    # The official automated-build index publishes SHA-1 digests for its SDKs.
    with archive.open("rb") as stream:
        digest = hashlib.sha1()
        while chunk := stream.read(1024 * 1024): digest.update(chunk)
    if digest.hexdigest() != spec["sha1"]:
        raise SystemExit("CEF SDK checksum mismatch: " + str(archive))
    sdk = ROOT / ".build/cef"
    marker = sdk / ".archive-sha1"
    if sdk.exists() and not marker.exists():
        version = json.loads((ROOT / "cef.json").read_text())["version"]
        header = sdk / "include/cef_version.h"
        if not header.exists() or ('#define CEF_VERSION "' + version + '"') not in header.read_text():
            raise SystemExit("Existing CEF SDK has a different version")
        marker.write_text(spec["sha1"])
    if marker.exists() and marker.read_text() != spec["sha1"]:
        raise SystemExit("CEF SDK changed; remove .build/cef before rebuilding")
    if not sdk.exists():
        unpack = ROOT / ".build/unpack"
        if unpack.exists(): shutil.rmtree(unpack)
        unpack.mkdir()
        with tarfile.open(archive) as data: data.extractall(unpack, filter="data")
        entries = list(unpack.iterdir())
        if len(entries) != 1 or not entries[0].is_dir():
            raise SystemExit("Expected one CEF SDK directory")
        entries[0].rename(sdk)
        unpack.rmdir()
        marker.write_text(spec["sha1"])
    return sdk

def run(command):
    print("+", " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), cwd=ROOT, check=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", default=host_arch())
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 2))
    parser.add_argument("--install", action="store_true", help="install runtime into the local plugin directory")
    args = parser.parse_args()
    if args.arch != host_arch(): parser.error("Build on a native runner for this architecture")
    sdk = cef_sdk(args.arch)
    build = ROOT / ".build/meson"
    command = ["meson", "setup", build, ROOT, "-Dcef_root=" + sdk.relative_to(ROOT).as_posix()]
    if (build / "meson-private/coredata.dat").exists(): command += ["--reconfigure"]
    run(command)
    run(["meson", "compile", "-C", build, "-j", args.jobs])
    runtime = ROOT / ".build/stage/runtime"
    if runtime.exists(): shutil.rmtree(runtime)
    runtime.mkdir(parents=True)
    suffix = ".dll" if platform.system() == "Windows" else ".so"
    output = build
    shutil.copy2(output / ("webbrowser-bridge" + suffix), runtime)
    if platform.system() == "Darwin":
        app = runtime / "webbrowser-host.app"
        (app / "Contents/MacOS").mkdir(parents=True)
        shutil.copy2(output / "webbrowser-host", app / "Contents/MacOS/webbrowser-host")
        shutil.copy2(ROOT / "native/Info.plist", app / "Contents/Info.plist")
        frameworks = app / "Contents/Frameworks"
        frameworks.mkdir(exist_ok=True)
        shutil.copytree(sdk / "Release/Chromium Embedded Framework.framework",
                        frameworks / "Chromium Embedded Framework.framework", symlinks=True)
        import plistlib
        for suffix, identifier in [("", ""), (" (Alerts)", ".alerts"), (" (GPU)", ".gpu"),
                                   (" (Plugin)", ".plugin"), (" (Renderer)", ".renderer")]:
            name = "WebBrowserHost Helper" + suffix
            helper = frameworks / (name + ".app/Contents")
            (helper / "MacOS").mkdir(parents=True)
            shutil.copy2(app / "Contents/MacOS/webbrowser-host", helper / ("MacOS/" + name))
            (helper / "Info.plist").write_bytes(plistlib.dumps({"CFBundleExecutable":name,
                "CFBundleIdentifier":"org.pragtical.webbrowser.host.helper" + identifier,
                "CFBundleName":name, "CFBundlePackageType":"APPL", "LSUIElement":True,
                "LSMinimumSystemVersion":"13.0", "CFBundleVersion":"0.1.0",
                "LSEnvironment":{"MallocNanoZone":"0"}}))
        run(["codesign", "--force", "--deep", "--sign", "-", app])
    else:
        executable = "webbrowser-host.exe" if platform.system() == "Windows" else "webbrowser-host"
        shutil.copy2(sdk / "Release/bootstrapc.exe" if platform.system() == "Windows" else output / executable,
                     runtime / executable)
        if platform.system() == "Windows": shutil.copy2(output / "webbrowser-host.dll", runtime)
        for directory in [sdk / "Release", sdk / "Resources"]:
            for path in directory.iterdir():
                if path.suffix in (".lib", ".pdb"): continue
                target = runtime / path.name
                if path.is_dir(): shutil.copytree(path, target, symlinks=True)
                else: shutil.copy2(path, target)
    # Binary SDKs include debugging sections that are unnecessary at runtime.
    # Keep all exported symbols and the complete CEF resources/locales.
    if platform.system() == "Linux":
        for path in runtime.iterdir():
            if path.is_file() and (".so" in path.name or path.name == "webbrowser-host"):
                run(["strip", "--strip-unneeded", path])
    licenses = runtime / "licenses"
    licenses.mkdir()
    for name in ["LICENSE.txt", "CREDITS.html"]: shutil.copy2(sdk / name, licenses)
    metadata = {"arch":args.arch,"cef":json.loads((ROOT / "cef.json").read_text())["version"]}
    (runtime / "build-info.json").write_text(json.dumps(metadata, indent=2) + "\n")
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    package = dist / ("webbrowser." + args.arch + (".zip" if platform.system() == "Windows" else ".tar.gz"))
    if platform.system() != "Windows":
        with tarfile.open(package, "w:gz", format=tarfile.GNU_FORMAT) as archive: archive.add(runtime, arcname="runtime")
    else:
        with zipfile.ZipFile(package, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            for path in sorted(runtime.rglob("*")):
                if path.is_file(): archive.write(path, path.relative_to(runtime.parent))
    if args.install:
        # Replace a development runtime without truncating a mapped shared library.
        destination = ROOT / "plugins/webbrowser/runtime"
        old = destination.with_name("runtime.old")
        if old.exists(): shutil.rmtree(old)
        if destination.exists(): destination.rename(old)
        shutil.copytree(runtime, destination, symlinks=True)
        if old.exists(): shutil.rmtree(old)
    checksum = hashlib.sha256()
    with package.open("rb") as stream:
        while chunk := stream.read(1024 * 1024): checksum.update(chunk)
    package.with_name(package.name + ".sha256").write_text(checksum.hexdigest() + "  " + package.name + "\n")
    print("Staged", package, flush=True)

if __name__ == "__main__": main()

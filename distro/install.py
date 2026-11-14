#!/usr/bin/env python3
"""Build unprivileged, validate a staged tree, then install only its checked files."""

import argparse
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

# Do not create bytecode files beside the repository's helper modules.
sys.dont_write_bytecode = True
import source


def staged_files(stage, prefix, data):
    script = prefix / "share/kwin/scripts" / source.SCRIPT_ID
    # ECM defaults are computed variables, not necessarily populated cache entries.
    # Accept the module's conventional lib, lib64 and multiarch Qt6 layouts only.
    qml_pattern = re.compile(r"lib(?:64)?/(?:[^/]+/){0,2}qml/com/github/luisbocanegra/trayicon/.+")
    kcm_pattern = re.compile(r"lib(?:64)?/(?:[^/]+/){0,2}plugins/kwin/effects/configs/kcm_minimize2tray\.so")
    # Only the two generated handbook files are allowed, not arbitrary documentation paths.
    handbook = prefix / "share/doc/HTML/en/kwin-minimize2tray"
    help_files = {handbook / "index.docbook", handbook / "index.cache.bz2"}
    files = []
    for path in sorted(stage.rglob("*")):
        source.require(not path.is_symlink(), f"Staged symlinks are not supported: {path}")
        if path.is_dir():
            continue
        source.require(path.is_file() and path.stat().st_nlink == 1, "Staged output contains a special file or hard link.")
        destination = Path("/") / path.relative_to(stage)
        source.require(destination.is_relative_to(prefix), f"Staged path escapes the prefix: {destination}")
        relative = destination.relative_to(prefix).as_posix()
        source.require(destination.is_relative_to(script) or destination in help_files or qml_pattern.fullmatch(relative) or
                       data["capabilities"]["kcm"] and kcm_pattern.fullmatch(relative),
                       f"Unexpected staged file: {destination}")
        files.append((path, destination))
    installed_script = stage / script.relative_to("/")
    metadata = json.loads((installed_script / "metadata.json").read_text())
    source.require(metadata["KPlugin"]["Id"] == source.SCRIPT_ID and metadata["KPlugin"]["Version"] == data["version"],
                   "Staged metadata has an unexpected identity/version.")
    provenance = installed_script / "source.json"
    source.write_new(provenance, (json.dumps(data, indent=2, sort_keys=True) + "\n").encode())
    files.append((provenance, script / "source.json"))
    source.require(sum(destination.name == "libtrayiconplugin.so" for _, destination in files) == 1, "The staged tray plugin is missing or duplicated.")
    source.require(not data["capabilities"]["kcm"] or
                   sum(destination.name == "kcm_minimize2tray.so" for _, destination in files) == 1, "The staged KCM is missing or duplicated.")
    return files


def check_destination(path, boundary):
    # Refuse existing leaf/ancestor symlinks instead of writing through them.
    source.require(path.is_relative_to(boundary), "Installation target escapes its prefix.")
    for candidate in (path, *path.parents):
        source.require(not candidate.is_symlink(), f"Installation target is a symlink: {candidate}")
        if candidate == boundary:
            break


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("system", "user"))
    source.source_arguments(parser)
    parser.add_argument("--prefix", type=Path, help="absolute installation prefix (default: /usr or ~/.local)")
    parser.add_argument("--destdir", type=Path, help="write a staged installation to a NEW directory; do not install or use sudo")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    parser.add_argument("--cmake-arg", action="append", default=[], help="additional -D option; use --cmake-arg=-DNAME=VALUE")
    args = parser.parse_args()
    source.require(os.geteuid() != 0, "Run the installer as a regular user; only the final system file copy uses sudo.")
    source.require(args.jobs > 0 and args.jobs <= 1024, "--jobs must be between 1 and 1024.")
    source.require(not args.archive or args.lock, "--archive requires --lock.")
    prefix = args.prefix or (Path("/usr") if args.mode == "system" else Path.home() / ".local")
    source.require(prefix.is_absolute() and ".." not in prefix.parts and prefix != Path("/"), "Use an absolute, non-root prefix without '..'.")
    prefix = prefix.resolve()
    if args.destdir:
        target = args.destdir.absolute()
        source.require(not target.exists() and not target.is_symlink(), "--destdir must name a new directory.")
    for option in args.cmake_arg:
        source.require(option.startswith("-D") and "=" in option, "--cmake-arg accepts only -DNAME=VALUE options.")
        key = option[2:].split("=", 1)[0].split(":", 1)[0]
        source.require(key not in {"CMAKE_INSTALL_PREFIX", "MINIMIZE2TRAY_VERSION", "BUILD_PLUGIN", "INSTALL_SCRIPT",
                                   "PACKAGE_SCRIPT", "PACKAGE_PLASMOID"} and not key.startswith("KDE_INSTALL_"),
                       f"{key} is controlled by the installer/source lock.")
    with tempfile.TemporaryDirectory(prefix="minimize2tray-install-", dir="/tmp") as directory:
        work = Path(directory)
        root, data, _ = source.prepare(args, work)
        print(f"Source: {data['owner']}/kwin-minimize2tray; {data['tag'] or data['mode']}; "
              f"revision {data['revision'] or data['source_id']}; version {data['version']}", flush=True)
        build, stage = work / "build", work / "stage"
        options = [f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DBUILD_PLUGIN=ON", "-DINSTALL_SCRIPT=ON",
                   "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=OFF", "-DPACKAGE_SCRIPT=OFF", "-DKDE_INSTALL_DATADIR=share"]
        if data["capabilities"]["version_option"]:
            options.append(f"-DMINIMIZE2TRAY_VERSION={data['version']}")
        if args.mode == "user":
            options += ["-DKDE_INSTALL_LIBDIR=lib", "-DKDE_INSTALL_QMLDIR=lib/qml", "-DKDE_INSTALL_PLUGINDIR=lib/plugins"]
        subprocess.run(["cmake", "-S", str(root), "-B", str(build), *options, *args.cmake_arg], check=True)
        subprocess.run(["cmake", "--build", str(build), "--parallel", str(args.jobs)], check=True)
        environment = dict(os.environ, DESTDIR=str(stage), CMAKE_INSTALL_MODE="COPY")
        subprocess.run(["cmake", "--install", str(build)], env=environment, check=True)
        files = staged_files(stage, prefix, data)
        if args.destdir:
            target = args.destdir.absolute()
            source.require(not target.exists() and not target.is_symlink(), "--destdir must name a new directory.")
            target.mkdir(parents=True)
            try:
                for path, destination in files:
                    output = target / destination.relative_to("/")
                    output.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(path, output)
                    output.chmod(0o755 if path.stat().st_mode & 0o111 else 0o644)
            except BaseException:
                shutil.rmtree(target)
                raise
            print(f"Staged installation: {target}; no system/user files were installed.")
        else:
            # Build scripts never run as root. Feed bytes from unprivileged reads to install(1),
            # so privileged copying cannot follow a replaced source path to a private system file.
            for _, destination in files:
                check_destination(destination, prefix)
            command = ["sudo", "install"] if args.mode == "system" else ["install"]
            for path, destination in files:
                mode = "0755" if path.stat().st_mode & 0o111 else "0644"
                subprocess.run([*command, "-D", "-m", mode, "/dev/stdin", str(destination)], input=path.read_bytes(), check=True)
            print(f"Installed version {data['version']} to {prefix}. Start a new Plasma session before live checks.")
        if args.mode == "user":
            print(f"QML_IMPORT_PATH must include {prefix}/lib/qml; QT_PLUGIN_PATH must include {prefix}/lib/plugins.")


if __name__ == "__main__":
    try:
        main()
    except (source.SourceError, OSError, ValueError, KeyError, TypeError, source.tarfile.TarError, subprocess.CalledProcessError) as error:
        sys.exit(f"minimize2tray install: {error}")

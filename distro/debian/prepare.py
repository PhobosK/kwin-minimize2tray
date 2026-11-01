#!/usr/bin/env python3
"""Prepare this fork's changelog-selected tag, or a specified commit, for dpkg-buildpackage."""

import argparse
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

# Reuse the installers' download/extraction guards without invoking their resolver.
sys.dont_write_bytecode = True
PACKAGING = Path(__file__).resolve().parent
sys.path.insert(0, str(PACKAGING.parent))
import source

NAME = "kwin6-script-minimize2tray"
REPOSITORY = "https://github.com/PhobosK/kwin-minimize2tray"


def changelog(field, contents):
    return subprocess.check_output(
        ["dpkg-parsechangelog", "--file", "-", "--show-field", field], input=contents
    ).decode("utf-8").strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new directory for the source tree and orig archive")
    parser.add_argument("--commit", help="full 40-digit commit SHA instead of the changelog's release tag")
    parser.add_argument("--archive", type=Path, help="use a matching, previously downloaded GitHub archive offline")
    args = parser.parse_args()

    original_changelog = (PACKAGING / "changelog").read_bytes()
    source.require(changelog("Source", original_changelog) == NAME, "Unexpected source package name in changelog.")
    package_version = changelog("Version", original_changelog)
    version_without_epoch = package_version.split(":", 1)[-1]
    source.require("-" in version_without_epoch, "Use a Debian revision, e.g. 0.5.0-1, in changelog.")
    version = version_without_epoch.rsplit("-", 1)[0]
    source.require(re.fullmatch(source.CORE, version), "The changelog upstream version must be X.Y.Z.")
    if args.commit:
        source.require(re.fullmatch(r"[0-9a-fA-F]{40}", args.commit), "--commit requires a full 40-digit SHA.")
        selected = args.commit.lower()
        ref = selected
        archive_root = "kwin-minimize2tray-" + selected
    else:
        selected = "v" + version
        ref = "refs/tags/" + selected
        archive_root = "kwin-minimize2tray-" + version
    url = f"{REPOSITORY}/archive/{ref}.tar.gz"

    # Claim a new output directory exclusively; never clean or overwrite an existing one.
    output = args.output.absolute()
    source.require(not output.resolve().is_relative_to(PACKAGING), "Output must be outside distro/debian.")
    output.mkdir()
    try:
        with tempfile.TemporaryDirectory(prefix=".prepare-", dir=output) as temporary:
            work = Path(temporary)
            archive = work / "source.tar.gz"
            if args.archive:
                source.require(args.archive.stat().st_size <= source.MAX_DOWNLOAD, "Source archive is too large.")
                shutil.copyfile(args.archive, archive)
            else:
                source.download(url, archive)
            tree = source.extract(archive, work / "unpacked")
            source.require(tree.name == archive_root, "Archive root does not match the selected tag/commit.")
            for name in ("CMakeLists.txt", "plugin/CMakeLists.txt", "plugin/configmodule.cpp",
                         "package/metadata.json", "LICENSE", "README.md", "CONTRIBUTING.md"):
                path = tree / name
                source.require(path.is_file() and path.resolve().is_relative_to(tree), f"Missing/unsafe source file: {name}")
            metadata = json.loads((tree / "package/metadata.json").read_text())
            source.require(metadata["KPlugin"]["Id"] == source.SCRIPT_ID, "Unexpected KWin script identity.")
            source.require(not (tree / "debian").exists() and not (tree / "debian").is_symlink(),
                           "Archive already contains a top-level debian directory.")
            shutil.copytree(PACKAGING, tree / "debian", ignore=shutil.ignore_patterns("prepare.py", "__pycache__", "*.pyc"))
            (tree / "debian/changelog").write_bytes(original_changelog)
            checksum = source.digest(archive)
            (tree / "debian/source-selection").write_text(
                f"Repository: {REPOSITORY}\nRef: {ref}\nVersion: {package_version}\nArchive-SHA256: {checksum}\n"
            )
            orig = output / f"{NAME}_{version}.orig.tar.gz"
            shutil.copyfile(archive, orig)
            tree.rename(output / f"{NAME}-{version}")
    except BaseException:
        shutil.rmtree(output)
        raise

    build = output / f"{NAME}-{version}"
    print(f"Prepared {selected}; package {package_version}; application {version}")
    print(f"Source SHA-256: {checksum}")
    print(f"Build: cd {shlex.quote(str(build))} && dpkg-buildpackage -us -uc")


if __name__ == "__main__":
    try:
        main()
    except (source.SourceError, OSError, ValueError, KeyError, TypeError,
            source.tarfile.TarError, subprocess.CalledProcessError) as error:
        sys.exit(f"minimize2tray Debian preparation: {error}")

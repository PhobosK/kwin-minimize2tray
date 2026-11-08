#!/usr/bin/env python3
"""Prepare verified Arch recipes or a Gentoo overlay for this fork's release."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
import tempfile

sys.dont_write_bytecode = True
import source

PACKAGING = Path(__file__).resolve().parent
ROOT = PACKAGING.parent
NAME = "kwin6-script-minimize2tray"
REPOSITORY = "https://github.com/PhobosK/kwin-minimize2tray"


def replace_once(contents, old, new):
    source.require(contents.count(old) == 1, f"Unexpected packaging template: {old}")
    return contents.replace(old, new, 1)


def prepare(args):
    metadata = json.loads((ROOT / "package/metadata.json").read_text())
    version = metadata["KPlugin"]["Version"]
    source.require(isinstance(version, str) and re.fullmatch(source.CORE, version), "Version must be X.Y.Z.")
    suffix = version
    if args.commit:
        source.require(re.fullmatch(r"[0-9a-fA-F]{40}", args.commit), "--commit requires a full 40-digit SHA.")
        suffix = args.commit.lower()
    if args.sha256:
        source.require(source.DIGEST.fullmatch(args.sha256), "--sha256 requires a lowercase SHA-256 digest.")
    archive_name = f"kwin-minimize2tray-{suffix}.tar.gz"
    archive_url = f"{REPOSITORY}/releases/download/v{version}/{archive_name}"
    output = args.output.absolute()
    source.require(not output.resolve().is_relative_to(PACKAGING), "Output must be outside distro.")
    output.mkdir()
    try:
        with tempfile.TemporaryDirectory(prefix=".prepare-", dir=output) as temporary:
            work = Path(temporary)
            archive = work / archive_name
            if args.archive:
                source.require(args.archive.stat().st_size <= source.MAX_DOWNLOAD, "Source archive is too large.")
                shutil.copyfile(args.archive, archive)
            else:
                source.require(not args.commit, "Commit builds require --archive from that exact commit.")
                source.download(archive_url, archive)
            checksum = source.digest(archive)
            source.require(not args.sha256 or checksum == args.sha256, "Source archive checksum mismatch.")
            tree = source.extract(archive, work / "unpacked")
            source.require(tree.name == f"kwin-minimize2tray-{suffix}", "Archive root does not match the selected source.")
            for name in ("CMakeLists.txt", "plugin/CMakeLists.txt", "plugin/configmodule.cpp", "package/metadata.json", "LICENSE", "README.md", "CONTRIBUTING.md"):
                path = tree / name
                source.require(path.is_file() and path.resolve().is_relative_to(tree), f"Missing/unsafe source file: {name}")
            selected = json.loads((tree / "package/metadata.json").read_text())["KPlugin"]
            source.require(selected["Id"] == source.SCRIPT_ID and selected["Version"] == version, "Source identity/version mismatch.")
            if args.format == "arch":
                recipe = (PACKAGING / "arch/PKGBUILD").read_text()
                source.require(f"pkgver={version}\n" in recipe, "PKGBUILD version mismatch.")
                recipe = replace_once(recipe, '_source_sha256=\'\'', f"_source_sha256='{checksum}'")
                recipe = replace_once(recipe, '_source_name="kwin-minimize2tray-${pkgver}.tar.gz"', f"_source_name='{archive_name}'")
                recipe = replace_once(recipe, '_source_dir="kwin-minimize2tray-${pkgver}"', f"_source_dir='kwin-minimize2tray-{suffix}'")
                (output / "PKGBUILD").write_text(recipe)
                shutil.copyfile(archive, output / archive_name)
            else:
                overlay = output / "overlay"
                shutil.copytree(PACKAGING / "gentoo", overlay)
                package = overlay / "kde-misc" / NAME
                recipe_path = package / f"{NAME}-{version}.ebuild"
                source.require(recipe_path.is_file(), "Ebuild version mismatch.")
                recipe = recipe_path.read_text()
                recipe = replace_once(recipe, 'SRC_URI="${HOMEPAGE}/releases/download/v${PV}/kwin-minimize2tray-${PV}.tar.gz"', f'SRC_URI="{archive_url}"')
                recipe = replace_once(recipe, 'S="${WORKDIR}/kwin-minimize2tray-${PV}"', f'S="${{WORKDIR}}/kwin-minimize2tray-{suffix}"')
                recipe_path.write_text(recipe)
                contents = archive.read_bytes()
                (package / "Manifest").write_text(
                    f"DIST {archive_name} {len(contents)} BLAKE2B {hashlib.blake2b(contents).hexdigest()} SHA512 {hashlib.sha512(contents).hexdigest()}\n"
                )
                (output / "distfiles").mkdir()
                shutil.copyfile(archive, output / "distfiles" / archive_name)
            (output / "source-selection").write_text(
                f"Repository: {REPOSITORY}\nRef: {args.commit.lower() if args.commit else 'refs/tags/v' + version}\n"
                f"Version: {version}\nArchive-SHA256: {checksum}\n"
            )
    except BaseException:
        shutil.rmtree(output)
        raise
    print(f"Prepared {args.format} packaging for {version}: {output}")
    print(f"Source SHA-256: {checksum}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("format", choices=("arch", "gentoo"))
    parser.add_argument("output", type=Path, help="new directory for the prepared packaging")
    parser.add_argument("--archive", type=Path, help="use an existing source archive offline")
    parser.add_argument("--commit", help="full commit SHA matching the supplied archive")
    parser.add_argument("--sha256", help="expected SHA-256 of the supplied source archive")
    prepare(parser.parse_args())


if __name__ == "__main__":
    try:
        main()
    except (source.SourceError, OSError, ValueError, KeyError, TypeError, source.tarfile.TarError) as error:
        sys.exit(f"minimize2tray packaging preparation: {error}")

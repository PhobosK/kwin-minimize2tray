#!/usr/bin/env python3
"""Resolve and prepare immutable Minimize2Tray sources without changing a checkout."""

import argparse
import gzip
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.parse
import urllib.request

if sys.version_info < (3, 12):
    sys.exit("Minimize2Tray source preparation requires Python 3.12 or newer.")

ROOT = Path(__file__).resolve().parent.parent
OWNERS = {"fork": "PhobosK", "upstream": "luisbocanegra"}
SCRIPT_ID = "com.github.luisbocanegra.minimize2tray"
SHA = re.compile(r"[0-9a-f]{40}")
DIGEST = re.compile(r"[0-9a-f]{64}")
CORE = r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
VERSION = re.compile(CORE + r"(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?")
STABLE = re.compile(r"v?" + CORE)
MAX_DOWNLOAD = 64 * 1024 * 1024


class SourceError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise SourceError(message)


def version(value):
    require(isinstance(value, str), "Version must be a string.")
    match = VERSION.fullmatch(value)
    require(len(value) <= 128 and match is not None, "Version must be SemVer, e.g. 1.2.3 or 1.2.3-rc.1.")
    prerelease = match.group(4)
    require(not prerelease or all(not part.isdigit() or part == "0" or not part.startswith("0")
                                 for part in prerelease.split(".")), "Numeric prerelease identifiers cannot have leading zeroes.")
    return value


def rpm_version(value):
    # RPM forbids hyphens in Version. Underscores preserve internal SemVer hyphens.
    core, separator, build = version(value).partition("+")
    core = core.replace("-", "~", 1).replace("-", "_")
    return core + ("+" + build.replace("-", "_") if separator else "")


def valid_tag(tag):
    require(isinstance(tag, str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+/-]{0,127}", tag) and
            ".." not in tag and "//" not in tag and not tag.endswith(("/", ".", ".lock")), "Invalid tag name.")
    return tag


def allowed_url(url):
    parts = urllib.parse.urlsplit(url)
    require(parts.scheme == "https" and parts.hostname in {"api.github.com", "github.com", "codeload.github.com"}
            and parts.port in (None, 443) and not parts.username and not parts.password, "Unsafe download redirect.")


class Redirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        allowed_url(newurl)
        return super().redirect_request(request, fp, code, msg, headers, newurl)


def download(url, target, limit=MAX_DOWNLOAD):
    allowed_url(url)
    headers = {"User-Agent": "kwin-minimize2tray-source-resolver"}
    if urllib.parse.urlsplit(url).hostname == "api.github.com":
        headers.update({"Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2026-03-10"})
    request = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.build_opener(Redirects()).open(request, timeout=30) as response, target.open("xb") as output:
            total = 0
            while block := response.read(1024 * 1024):
                total += len(block)
                require(total <= limit, "Download exceeds the size limit.")
                output.write(block)
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        raise SourceError(f"Download failed for {url}: {error}. No release fallback was selected.") from error


def api(owner, suffix):
    with tempfile.TemporaryDirectory(prefix="minimize2tray-api-", dir="/tmp") as directory:
        path = Path(directory) / "response.json"
        download(f"https://api.github.com/repos/{owner}/kwin-minimize2tray/{suffix}", path, 8 * 1024 * 1024)
        return json.loads(path.read_bytes())


def tags(owner):
    result = {}
    for page in range(1, 101):
        entries = api(owner, f"tags?per_page=100&page={page}")
        require(isinstance(entries, list), "GitHub returned an invalid tag list.")
        for entry in entries:
            name, revision = entry["name"], entry["commit"]["sha"]
            valid_tag(name)
            require(isinstance(revision, str) and SHA.fullmatch(revision), "GitHub returned an invalid commit SHA.")
            require(name not in result or result[name] == revision, "Tags changed during discovery; retry.")
            result[name] = revision
        if len(entries) < 100:
            return result
    raise SourceError("Tag discovery exceeds the 10000-tag limit; no partial list will be used.")


def choose_tag(available, selected=None, selected_version=None):
    if selected:
        require(selected in available, f"Tag {selected!r} does not exist in the selected repository.")
        return selected
    if selected_version:
        candidates = [tag for tag in available if tag.removeprefix("v") == selected_version]
        require(candidates, f"No tag matches version {selected_version} in the selected repository.")
    else:
        candidates = [tag for tag in available if STABLE.fullmatch(tag)]
        require(candidates, "No stable X.Y.Z or vX.Y.Z release tag exists; use --tag and --app-version explicitly.")
        latest = max(tuple(map(int, STABLE.fullmatch(tag).groups())) for tag in candidates)
        candidates = [tag for tag in candidates if tuple(map(int, STABLE.fullmatch(tag).groups())) == latest]
        ignored = sorted(set(available) - {tag for tag in available if STABLE.fullmatch(tag)})
        if ignored:
            print(f"Ignoring non-stable/non-SemVer tags for latest selection: {', '.join(ignored[:10])}"
                  + (" ..." if len(ignored) > 10 else ""), file=sys.stderr)
    require(len({available[tag] for tag in candidates}) == 1,
            "Equivalent version tags point at different commits; choose an exact --tag.")
    return sorted(candidates, key=lambda tag: (not tag.startswith("v"), tag))[0]


def local_version(work):
    build = work / "version-build"
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(build), "-DBUILD_PLUGIN=OFF", "-DINSTALL_SCRIPT=OFF"],
                   check=True, stdout=sys.stderr)
    return version(json.loads((build / "metadata.json").read_text())["KPlugin"]["Version"])


def resolve(args, work):
    if args.app_version:
        version(args.app_version)
    if args.tag:
        valid_tag(args.tag)
    if args.version:
        version(args.version)
    if args.lock:
        require(Path(args.lock).stat().st_size <= 65536, "Source lock exceeds 64 KiB.")
        data = json.loads(Path(args.lock).read_text())
        validate_lock(data)
        require(not args.source or args.source == data["source"], "--source conflicts with the locked source.")
        require(not args.app_version, "A lock fixes the application version; --app-version cannot override it.")
        return data
    selected_source = args.source or "fork"
    owner = OWNERS[selected_source]
    data = {"format": 1, "source": selected_source, "owner": owner, "mode": "local" if args.local else "tag",
            "tag": None, "revision": None}
    if args.local:
        require(selected_source == "fork", "--local uses this fork's checkout; it cannot select upstream.")
        data["version"] = version(args.app_version) if args.app_version else local_version(work)
    elif args.revision:
        revision = args.revision.lower()
        require(SHA.fullmatch(revision), "--revision requires a full 40-digit commit SHA.")
        require(args.app_version, "A revision needs an explicit --app-version; no ancestral tag is guessed.")
        require(api(owner, f"commits/{revision}").get("sha") == revision, "Commit is not available in the selected repository.")
        data.update(mode="revision", revision=revision, version=version(args.app_version))
    else:
        available = tags(owner)
        tag = choose_tag(available, valid_tag(args.tag) if args.tag else None,
                         version(args.version) if args.version else None)
        # Resolve once, then download only the commit archive, never the mutable tag URL.
        return resolve_remote(args, data, tag, available[tag])
    data["rpm_version"] = rpm_version(data["version"])
    return data


def resolve_remote(args, data, tag, expected_revision):
    # Recheck the exact ref, including annotated tags, after selecting its version.
    ref = api(data["owner"], "git/ref/tags/" + urllib.parse.quote(tag, safe=""))["object"]
    for _ in range(8):
        require(isinstance(ref.get("sha"), str) and SHA.fullmatch(ref["sha"]), "Invalid Git object SHA.")
        if ref.get("type") == "commit":
            break
        require(ref.get("type") == "tag", "Tag does not identify a commit.")
        ref = api(data["owner"], "git/tags/" + ref["sha"])["object"]
    else:
        raise SourceError("Annotated tag nesting exceeds the limit.")
    require(ref["sha"] == expected_revision, "Tag changed during discovery; retry.")
    derived = tag.removeprefix("v")
    require(args.app_version or VERSION.fullmatch(derived), "Non-SemVer tags require an explicit --app-version.")
    data.update(tag=tag, revision=ref["sha"], version=version(args.app_version or derived))
    data["rpm_version"] = rpm_version(data["version"])
    return data


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def extract(archive, target):
    """Extract only a bounded, single-root tree, with no writes through links."""
    require(archive.stat().st_size <= MAX_DOWNLOAD, "Source archive is too large.")
    target.mkdir()
    with tarfile.open(archive, "r:gz") as stream:
        members, total = [], 0
        for member in stream:
            total += member.size
            require(len(members) < 20000 and 0 <= member.size <= 32 * 1024 * 1024 and total <= 256 * 1024 * 1024,
                    "Source archive exceeds the extraction limits.")
            members.append(member)
        require(members, "Source archive is empty.")
        paths, links, roots = set(), set(), set()
        for member in members:
            path = PurePosixPath(member.name)
            require(not path.is_absolute() and path.parts and ".." not in path.parts and
                    "\\" not in member.name and not any(ord(c) < 32 for c in member.name), "Unsafe archive path.")
            require(path not in paths, "Duplicate archive path.")
            paths.add(path)
            roots.add(path.parts[0])
            require(member.isdir() or member.isfile() or member.issym(), "Unsupported archive entry (including hard links).")
            if member.issym():
                links.add(path)
                require(not PurePosixPath(member.linkname).is_absolute(), "Absolute archive symlink.")
                resolved = (target / str(path.parent) / member.linkname).resolve()
                require(resolved.is_relative_to(target.resolve() / path.parts[0]), "Archive symlink escapes the source tree.")
        require(len(roots) == 1, "Source archive must have one top-level directory.")
        for path in paths:
            require(not any(parent in links for parent in path.parents), "Archive entry writes through a symlink.")
        # Python's data filter adds platform-specific path and link protections.
        stream.extractall(target, members=members, filter="data")
    root = target / roots.pop()
    require(root.is_dir() and not root.is_symlink(), "Invalid archive root.")
    return root


def snapshot(target):
    target.mkdir()
    # Copy only build inputs, including unpublished edits; never copy .git, credentials or developer artifacts.
    for name in ("CMakeLists.txt", "plugin", "package", "cmake", "handbook", "LICENSE", "README.md", "CONTRIBUTING.md"):
        source = ROOT / name
        if not source.exists():
            continue
        for path in ([source] if not source.is_dir() else [source, *source.rglob("*")]):
            require(not path.is_symlink(), f"Local build input is a symlink: {path.relative_to(ROOT)}")
        if source.is_dir():
            shutil.copytree(source, target / name)
        else:
            shutil.copyfile(source, target / name)
    return target


def inspect_source(root):
    for name in ("CMakeLists.txt", "plugin/CMakeLists.txt", "package/metadata.json", "LICENSE"):
        require((root / name).is_file() and (root / name).resolve().is_relative_to(root.resolve()), f"Missing/unsafe source file: {name}")
    cmake = (root / "CMakeLists.txt").read_text()
    plugin = (root / "plugin/CMakeLists.txt").read_text()
    require(json.loads((root / "package/metadata.json").read_text())["KPlugin"]["Id"] == SCRIPT_ID, "Unexpected script identity.")
    require("BUILD_PLUGIN" in cmake and "INSTALL_SCRIPT" in cmake, "Unsupported source build options.")
    return {"version_option": "MINIMIZE2TRAY_VERSION" in cmake,
            "kpackage": "kpackage_install_package" in cmake,
            "kcm": bool(re.search(r"add_library\s*\(\s*kcm_minimize2tray\b", plugin))}


def archive_tree(root, output):
    with output.open("xb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", filename="", mtime=0) as zipped, tarfile.open(fileobj=zipped, mode="w") as stream:
        for path in [root, *sorted(root.rglob("*"))]:
            name = "kwin-minimize2tray" + ("/" + path.relative_to(root).as_posix() if path != root else "")
            info = stream.gettarinfo(str(path), arcname=name)
            info.uid = info.gid = info.mtime = 0
            info.uname = info.gname = ""
            info.mode = 0o755 if info.isdir() or path.lstat().st_mode & 0o111 else 0o644
            if info.isfile():
                with path.open("rb") as contents:
                    stream.addfile(info, contents)
            else:
                stream.addfile(info)


def stem(data):
    return f"kwin6-script-minimize2tray-{data['owner']}-{data['version']}-{data['source_id']}"


def validate_lock(data):
    fields = {"format", "source", "owner", "mode", "tag", "revision", "version", "rpm_version", "source_id",
              "download_sha256", "capabilities", "archive", "archive_sha256"}
    require(isinstance(data, dict) and set(data) == fields and type(data.get("format")) is int and
            data["format"] == 1 and data.get("source") in OWNERS, "Invalid source lock format.")
    require(data.get("owner") == OWNERS[data["source"]] and data.get("mode") in {"tag", "revision", "local"}, "Invalid locked source identity.")
    version(data["version"])
    require(data["rpm_version"] == rpm_version(data["version"]), "Inconsistent locked package version.")
    if data["mode"] == "local":
        require(data["source"] == "fork" and data.get("revision") is None and data["download_sha256"] is None and
                re.fullmatch(r"local\.[0-9a-f]{64}", data["source_id"]), "Invalid locked local identity.")
    else:
        require(isinstance(data.get("revision"), str) and SHA.fullmatch(data["revision"]) and
                data["source_id"] == data["revision"], "Invalid locked commit.")
        require(isinstance(data.get("download_sha256"), str) and DIGEST.fullmatch(data["download_sha256"]), "Invalid download checksum.")
    if data["mode"] == "tag":
        valid_tag(data["tag"])
    else:
        require(data.get("tag") is None, "Unexpected locked tag.")
    require(data.get("archive") == stem(data) + ".tar.gz" and isinstance(data.get("archive_sha256"), str) and
            DIGEST.fullmatch(data["archive_sha256"]), "Invalid locked archive identity/checksum.")
    require(set(data.get("capabilities", {})) == {"version_option", "kpackage", "kcm"} and
            all(type(value) is bool for value in data["capabilities"].values()), "Invalid locked source capabilities.")


def prepare(args, work):
    data = resolve(args, work)
    if args.archive:
        require(args.lock, "--archive requires the corresponding --lock file.")
        archive = Path(args.archive)
        require(archive.stat().st_size <= MAX_DOWNLOAD, "Source archive is too large.")
        require(digest(archive) == data["archive_sha256"], "Source archive checksum does not match the lock.")
        root = extract(archive, work / "extracted")
        internal = json.loads((root / "minimize2tray-source.json").read_text())
        require(internal == {key: value for key, value in data.items() if key not in {"archive", "archive_sha256"}}, "Archive provenance does not match the lock.")
        require(inspect_source(root) == data["capabilities"], "Archive capabilities differ from the lock.")
        require(json.loads((root / "package/metadata.json").read_text())["KPlugin"]["Version"] == data["version"],
                "Prepared metadata version differs from the lock.")
        return root, data, archive
    require(not args.lock or data["mode"] != "local", "A local lock requires its --archive; the checkout may have changed.")
    if data["mode"] == "local":
        root = snapshot(work / "source")
        original = work / "snapshot.tar.gz"
        archive_tree(root, original)
        data["source_id"] = "local." + digest(original)
        data["download_sha256"] = None
    else:
        downloaded = work / "github.tar.gz"
        download(f"https://github.com/{data['owner']}/kwin-minimize2tray/archive/{data['revision']}.tar.gz", downloaded)
        checksum = digest(downloaded)
        require(not args.lock or checksum == data["download_sha256"], "Downloaded bytes differ from the lock; use its matching --archive.")
        data["download_sha256"] = checksum
        data["source_id"] = data["revision"]
        root = extract(downloaded, work / "extracted")
    capabilities = inspect_source(root)
    require(not args.lock or capabilities == data["capabilities"], "Source capabilities differ from the lock.")
    data["capabilities"] = capabilities
    metadata_path = root / "package/metadata.json"
    metadata = json.loads(metadata_path.read_text())
    metadata["KPlugin"]["Version"] = data["version"]
    metadata_path.write_text(json.dumps(metadata, ensure_ascii=False, indent=4) + "\n")
    internal = {key: value for key, value in data.items() if key not in {"archive", "archive_sha256"}}
    (root / "minimize2tray-source.json").write_text(json.dumps(internal, indent=2, sort_keys=True) + "\n")
    archive = work / (stem(data) + ".tar.gz")
    archive_tree(root, archive)
    checksum = digest(archive)
    require(not args.lock or checksum == data["archive_sha256"], "Prepared source differs from the lock; use its matching --archive.")
    data.update(archive=archive.name, archive_sha256=checksum)
    validate_lock(data)
    return root, data, archive


def source_arguments(parser):
    parser.add_argument("--source", choices=OWNERS, help="repository (default: fork)")
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--version", help="select a release version, with or without a leading-v tag")
    selection.add_argument("--tag", help="select an exact tag")
    selection.add_argument("--revision", help="select a full commit SHA; requires --app-version")
    selection.add_argument("--local", action="store_true", help="snapshot this fork's unpublished build inputs")
    selection.add_argument("--lock", help="replay a prepared source lock without tag discovery")
    parser.add_argument("--archive", help="use the prepared archive named by --lock, entirely offline")
    parser.add_argument("--app-version", help="explicit application/package version override")


def write_new(path, contents):
    stream = path.open("xb")
    try:
        with stream:
            stream.write(contents)
    except BaseException:
        path.unlink()
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("resolve", "prepare"))
    source_arguments(parser)
    parser.add_argument("--output", type=Path, help="artifact directory (prepare)")
    args = parser.parse_args()
    require(not args.archive or args.lock, "--archive requires --lock.")
    require(args.action == "resolve" or args.output, "prepare requires --output.")
    with tempfile.TemporaryDirectory(prefix="minimize2tray-source-", dir="/tmp") as directory:
        work = Path(directory)
        if args.action == "resolve":
            data = resolve(args, work)
        else:
            _, data, archive = prepare(args, work)
            output = args.output.resolve()
            output.mkdir(parents=True, exist_ok=True)
            files = {data["archive"]: archive.read_bytes(),
                     stem(data) + ".lock.json": (json.dumps(data, indent=2, sort_keys=True) + "\n").encode()}
            require(not any((output / name).exists() or (output / name).is_symlink() for name in files), "Output artifacts already exist; choose a new output directory.")
            created = []
            try:
                for name, contents in files.items():
                    write_new(output / name, contents)
                    created.append(output / name)
            except BaseException:
                for path in created:
                    path.unlink()
                raise
        print(json.dumps(data, indent=2, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (SourceError, OSError, ValueError, KeyError, TypeError, tarfile.TarError, subprocess.CalledProcessError) as error:
        sys.exit(f"minimize2tray source: {error}")

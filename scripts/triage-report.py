#!/usr/bin/env python3
"""Summarizes a vkpcnx debug report and symbolizes its crashes.

    scripts/triage-report.py vkpcnx-report-20261002-123456.zip
    scripts/triage-report.py report.zip --fetch          # download the build's symbols with gh
    scripts/triage-report.py report.zip --symbols DIR    # symbols already on disk
    scripts/triage-report.py some.log                    # a single log file works too

A report is what the app's "Create debug report" button (or
`vkpcnx --create-report`) writes: report.txt, logs/*.log, settings.json and
crash_reports/*. Crash sections in the logs ("*** CRASH") print frames as
module+0xoffset; frames in vkpcnx itself are resolved against the symbols CI
archived for that commit (artifact / release asset vkpcnx-<platform>-symbols).

Symbol files looked for in --symbols / the fetch cache:
  switch-aarch64  vkpcnx.elf                      (aarch64-none-elf-addr2line, llvm-addr2line or Docker)
  linux-*         vkpcnx.debug or vkpcnx          (addr2line)
  windows-*       vkpcnx.exe.debug or vkpcnx.exe  (addr2line)
  macos-*         vkpcnx.dSYM or vkpcnx           (atos)
"""

import argparse
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile
from collections import Counter
from pathlib import Path

DEFAULT_REPO = "m4l3vich/vkpcnx"
CACHE = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "vkpcnx-symbols"
MAIN_MODULES = {"vkpcnx", "vkpcnx.nro", "vkpcnx.exe", "vkpcnx.elf"}

LINE_RE = re.compile(r"^(\d\d:\d\d:\d\d\.\d{3}) ([EWIDV]) (\S+)\s+(.*)$")
FRAME_RE = re.compile(r"^\s+bt #(\d+) (0x[0-9a-f]+) (\S+?)\+(0x[0-9a-f]+)(?: (\S+))?\s*$")
HEADER_RE = re.compile(r"^([a-z][a-z ]+):\s+(.*)$")


# ---------------------------------------------------------------- loading


def load(path):
    """Returns {name: text} for a report zip, a directory or a single file."""
    p = Path(path)
    files = {}
    if p.is_dir():
        for f in sorted(p.rglob("*")):
            if f.is_file():
                files[str(f.relative_to(p))] = f.read_text(errors="replace")
    elif zipfile.is_zipfile(p):
        with zipfile.ZipFile(p) as z:
            for name in z.namelist():
                files[name] = z.read(name).decode("utf-8", errors="replace")
    else:
        files["logs/" + p.name] = p.read_text(errors="replace")
    return files


def parse_header(text):
    """Key/value lines between the '=== vkpcnx log' banner and '==='."""
    info = {}
    lines = text.splitlines()
    if not lines or not lines[0].startswith("=== vkpcnx log"):
        return info
    info["started"] = lines[0].strip("= ").replace("vkpcnx log, started ", "")
    for line in lines[1:]:
        if line.startswith("==="):
            break
        m = HEADER_RE.match(line)
        if m:
            info[m.group(1)] = m.group(2).strip()
    return info


def crash_sections(text):
    """Each '*** CRASH' block up to the next log line or blank-line gap."""
    out = []
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        if lines[i].startswith("*** CRASH"):
            block = [lines[i]]
            i += 1
            while i < len(lines) and lines[i].startswith(" "):
                block.append(lines[i])
                i += 1
            out.append((i, block))
        else:
            i += 1
    return out


# ---------------------------------------------------------------- symbols


def commit_of(git):
    sha = git.replace("-dirty", "")
    return sha if re.fullmatch(r"[0-9a-f]{7,40}", sha) else None


def gh(*args):
    return subprocess.run(["gh", *args], capture_output=True, text=True, check=False)


def artifact_platform(platform):
    """CI artifact naming: one universal macOS build, one Switch build."""
    if platform.startswith("switch"):
        return "switch"
    if platform.startswith("macos"):
        return "macos"
    return platform


def fetch_symbols(repo, sha, platform):
    """Downloads vkpcnx-<platform>-symbols for `sha` from CI artifacts or a release."""
    dest = CACHE / sha / platform
    if dest.exists() and any(dest.iterdir()):
        return dest
    if not shutil.which("gh"):
        print("  (gh not installed, can't fetch symbols)")
        return None
    name = f"vkpcnx-{artifact_platform(platform)}-symbols"
    dest.mkdir(parents=True, exist_ok=True)

    runs = gh("run", "list", "-R", repo, "--commit", sha, "--json", "databaseId,status,conclusion")
    for run in json.loads(runs.stdout or "[]"):
        r = gh("run", "download", str(run["databaseId"]), "-R", repo, "-n", name, "-D", str(dest))
        if r.returncode == 0:
            unpack_archives(dest)
            return dest

    releases = gh("release", "list", "-R", repo, "--limit", "50", "--json", "tagName")
    for rel in json.loads(releases.stdout or "[]"):
        tag = rel["tagName"]
        c = gh("api", f"repos/{repo}/commits/{tag}", "--jq", ".sha")
        if c.stdout.strip().startswith(sha):
            r = gh("release", "download", tag, "-R", repo, "-p", f"{name}*", "-D", str(dest))
            if r.returncode == 0:
                unpack_archives(dest)
                return dest
    print(f"  (no {name} found for {sha} in CI runs or releases of {repo})")
    return None


def unpack_archives(d):
    for f in list(d.iterdir()):
        if f.name.endswith((".tar.gz", ".tgz", ".zip")):
            shutil.unpack_archive(str(f), str(d))


def find_symbol_file(d, platform):
    if d is None:
        return None
    candidates = {
        "switch": ["vkpcnx.elf"],
        "linux": ["vkpcnx.debug", "vkpcnx"],
        "windows": ["vkpcnx.exe.debug", "vkpcnx.exe"],
        "macos": ["vkpcnx.dSYM", "vkpcnx.app.dSYM", "vkpcnx"],
    }[platform.split("-")[0]]
    for name in candidates:
        hits = sorted(Path(d).rglob(name))
        if hits:
            return hits[0]
    return None


def pe_image_base(path):
    with open(path, "rb") as f:
        data = f.read(4096)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    return struct.unpack_from("<Q", data, pe + 24 + 24)[0] if magic == 0x20B else \
        struct.unpack_from("<I", data, pe + 24 + 28)[0]


def addr2line_tool(platform):
    if platform.startswith("switch"):
        for tool in ["aarch64-none-elf-addr2line", "llvm-addr2line"]:
            if shutil.which(tool):
                return [tool]
        devkit = os.environ.get("DEVKITPRO", "/opt/devkitpro")
        local = Path(devkit) / "devkitA64/bin/aarch64-none-elf-addr2line"
        if local.exists():
            return [str(local)]
        if shutil.which("docker"):
            return ["docker"]  # resolved in symbolize()
        return None
    for tool in ["addr2line", "llvm-addr2line"]:
        if shutil.which(tool):
            return [tool]
    return None


def symbolize(platform, symfile, offsets, link_base):
    """{offset: "function at file:line"} for module-relative offsets."""
    if not symfile or not offsets:
        return {}
    offsets = sorted(set(offsets))
    if platform.startswith("macos"):
        target = symfile
        if symfile.suffix == ".dSYM":
            dwarf = list((symfile / "Contents/Resources/DWARF").glob("*"))
            target = dwarf[0] if dwarf else symfile
        arch = "arm64" if platform.endswith("arm64") else "x86_64"
        addrs = [hex(link_base + o) for o in offsets]
        r = subprocess.run(["atos", "-o", str(target), "-arch", arch, *addrs],
                           capture_output=True, text=True)
        return dict(zip(offsets, r.stdout.splitlines()))

    base = 0
    if platform.startswith("windows"):
        base = pe_image_base(symfile)
    tool = addr2line_tool(platform)
    if not tool:
        print("  (no addr2line found; install binutils / devkitA64, or LLVM)")
        return {}
    addrs = [hex(base + o) for o in offsets]
    if tool == ["docker"]:
        cmd = ["docker", "run", "--rm", "-v", f"{symfile.resolve().parent}:/sym", "devkitpro/devkita64",
               "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line", "-e", f"/sym/{symfile.name}", "-f", "-C", "-i", "-a", *addrs]
    else:
        cmd = [*tool, "-e", str(symfile), "-f", "-C", "-i", "-a", *addrs]
    r = subprocess.run(cmd, capture_output=True, text=True)
    # -a prints each address, then function/location pairs (several when inlined)
    result, current, lines = {}, None, []
    by_addr = {base + o: o for o in offsets}
    for line in r.stdout.splitlines():
        if re.fullmatch(r"0x[0-9a-f]+", line):
            if current is not None:
                result[current] = format_frames(lines)
            current, lines = by_addr.get(int(line, 16)), []
        else:
            lines.append(line)
    if current is not None:
        result[current] = format_frames(lines)
    return result


def format_frames(lines):
    pairs = [f"{lines[i]} at {short_path(lines[i + 1])}" for i in range(0, len(lines) - 1, 2)]
    return " <- inlined in ".join(pairs) if pairs else "?"


def short_path(p):
    for marker in ("/vkpcnx/src/", "/vkpcnx/include/", "/library/", "/_deps/"):
        i = p.find(marker)
        if i >= 0:
            return p[i + 1:]
    return p


# ---------------------------------------------------------------- atmosphère


def atmosphere_offsets(text, build_ids):
    """Module-relative offsets of addresses in an Atmosphère report that fall
    inside the vkpcnx module (matched by its Module/Build Id)."""
    lowered = text.lower()
    start = end = None
    for m in re.finditer(r"address:\s+([0-9a-f]{8,16})-([0-9a-f]{8,16})(.*?)(?:module|build) id:\s+([0-9a-f]+)",
                         lowered, re.S):
        if any(m.group(4).startswith(b[:32]) for b in build_ids):
            start, end = int(m.group(1), 16), int(m.group(2), 16)
    if start is None:
        return None, []
    offsets = []
    for m in re.finditer(r"(?:0x)?([0-9a-f]{10,16})\b", lowered):
        v = int(m.group(1), 16)
        if start <= v < end:
            offsets.append(v - start)
    return start, sorted(set(offsets))


# ---------------------------------------------------------------- report


def summarize_log(name, text, args, symbols_cache, current=False):
    info = parse_header(text)
    lines = text.splitlines()
    parsed = [LINE_RE.match(l) for l in lines]
    levels = Counter(m.group(2) for m in parsed if m)
    print(f"\n## {name}")
    if info:
        print(f"  started {info.get('started')}, {info.get('version')} {info.get('git')} "
              f"{info.get('platform')} {info.get('build type')}")
        prev = info.get("previous run")
        if prev:
            print(f"  previous run: {prev}")
    print(f"  {len(lines)} lines: {levels.get('E', 0)} errors, {levels.get('W', 0)} warnings")

    # Most frequent warnings/errors, numbers collapsed so repeats group together
    noisy = Counter()
    first_seen = {}
    for m in parsed:
        if m and m.group(2) in "EW":
            key = m.group(2) + " " + re.sub(r"\d+", "N", m.group(4))[:140]
            noisy[key] += 1
            first_seen.setdefault(key, m.group(1))
    if noisy:
        print("  top warnings/errors:")
        for key, n in noisy.most_common(8):
            print(f"    {n:>5}x  (first {first_seen[key]})  {key}")

    for m in parsed:
        if m and m.group(4).startswith("Watchdog:"):
            print(f"  {m.group(1)} {m.group(4)}")

    stats = [m for m in parsed if m and m.group(4).startswith("Stats:")]
    if stats:
        lossy = [m for m in stats if not re.search(r"rtp lost 0/", m.group(4))]
        print(f"  stream stats: {len(stats)} samples, {len(lossy)} with RTP loss; last:")
        print(f"    {stats[-1].group(1)} {stats[-1].group(4)}")

    crashes = crash_sections(text)
    if not crashes:
        tail = lines[-50:]
        terminated = [l for l in tail if l.startswith("*** terminated by")]
        if current:
            print("  (the run that created this report; it was still going)")
        elif terminated:
            print(f"  {terminated[-1].strip('* ')} (closed by the user or the OS)")
        elif lines and not any("Log: clean exit" in l for l in tail):
            print("  no clean exit and no crash section: hang, kill or power loss; last lines:")
            for l in lines[-args.context:]:
                print("    " + l)
        return info

    platform = info.get("platform", "")
    sha = commit_of(info.get("git", ""))
    symfile = None
    if args.symbols:
        symfile = find_symbol_file(Path(args.symbols), platform)
    elif args.fetch and sha and platform:
        key = (sha, platform)
        if key not in symbols_cache:
            symbols_cache[key] = find_symbol_file(fetch_symbols(args.repo, sha, platform), platform)
        symfile = symbols_cache[key]
    if (info.get("git", "").endswith("-dirty")):
        print("  note: built from a dirty tree; CI symbols may not match exactly")

    for end, block in crashes:
        print(f"\n  {block[0]}")
        print("  context before the crash:")
        start = max(0, end - len(block) - args.context)
        for l in lines[start:end - len(block)]:
            print("    " + l)
        link_base = 0
        offsets = []
        for l in block:
            m = re.search(r"link-base (0x[0-9a-f]+)", l)
            if m:
                link_base = int(m.group(1), 16)
            f = FRAME_RE.match(l)
            if f and f.group(3) in MAIN_MODULES:
                offsets.append(int(f.group(4), 16))
            for m in re.finditer(r"(?:pc|lr)=\+(0x[0-9a-f]+)", l):
                offsets.append(int(m.group(1), 16))
        resolved = symbolize(platform, symfile, offsets, link_base)
        print("  crash:")
        for l in block[1:]:
            f = FRAME_RE.match(l)
            extra = ""
            if f and f.group(3) in MAIN_MODULES:
                extra = resolved.get(int(f.group(4), 16), "")
            elif f and f.group(5):
                extra = f.group(5)
            for m in re.finditer(r"(pc|lr)=\+(0x[0-9a-f]+)", l):
                r = resolved.get(int(m.group(2), 16))
                if r:
                    extra += f"  {m.group(1)}: {r}"
            print(f"    {l.strip()}" + (f"\n        {extra}" if extra else ""))
        if not symfile:
            print("  (frames not symbolized: pass --fetch or --symbols DIR)")
    return info


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("report", help="report .zip, an unpacked report directory, or a .log file")
    ap.add_argument("--symbols", help="directory with the build's symbol files")
    ap.add_argument("--fetch", action="store_true", help="download symbols for the build with gh")
    ap.add_argument("--repo", default=DEFAULT_REPO, help=f"GitHub repo for --fetch (default {DEFAULT_REPO})")
    ap.add_argument("--context", type=int, default=25, help="log lines shown before a crash / at the end")
    ap.add_argument("--extract", metavar="DIR", help="also unpack the report into DIR")
    args = ap.parse_args()

    files = load(args.report)
    if args.extract:
        out = Path(args.extract)
        for name, text in files.items():
            (out / name).parent.mkdir(parents=True, exist_ok=True)
            (out / name).write_text(text)

    print(f"# {Path(args.report).name}")
    if "report.txt" in files:
        print(files["report.txt"].split("\nfiles:")[0].rstrip())

    symbols_cache = {}
    build_ids, platform, sha = set(), None, None
    m = re.search(r"^current log:\s+(\S+)", files.get("report.txt", ""), re.M)
    current_log = "logs/" + m.group(1) if m else None
    for name in sorted((n for n in files if n.startswith("logs/")), reverse=True):
        info = summarize_log(name, files[name], args, symbols_cache, current=name == current_log)
        if info.get("build id", "unknown") != "unknown":
            build_ids.add(info["build id"].lower())
            platform = platform or info.get("platform")
            sha = sha or commit_of(info.get("git", ""))

    for name in sorted(n for n in files if n.startswith("crash_reports/")):
        text = files[name]
        print(f"\n## {name}")
        if name.endswith((".ips", ".crash")):
            # macOS: the system report already has symbol names for each thread
            print("  macOS crash report; see the faulting thread's frames in the file")
            continue
        base, offsets = atmosphere_offsets(text, build_ids)
        if base is None:
            print("  no vkpcnx module with a matching build id in this report")
            continue
        print(f"  vkpcnx module at {base:#x}; {len(offsets)} addresses inside it")
        symfile = None
        if args.symbols:
            symfile = find_symbol_file(Path(args.symbols), "switch-aarch64")
        elif args.fetch and sha:
            symfile = find_symbol_file(fetch_symbols(args.repo, sha, "switch-aarch64"), "switch-aarch64")
        for off, where in sorted(symbolize("switch-aarch64", symfile, offsets, 0).items()):
            print(f"    +{off:#x}  {where}")

    if "settings.json" in files:
        print("\n## settings.json")
        print(io.StringIO(files["settings.json"]).read().rstrip())


if __name__ == "__main__":
    sys.exit(main())

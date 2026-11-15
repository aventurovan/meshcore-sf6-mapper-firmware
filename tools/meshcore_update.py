#!/usr/bin/env python3
"""Przeniesienie Companion + Mapper na najnowsze oficjalne wydanie MeshCore companion.

Uruchamiane przez workflow meshcore-update.yml. Kroki:
  1. Najnowszy tag companion-vX.Y.Z w meshcore-dev/MeshCore vs. wersja z build.yml (komentarz przy MESHCORE_REF).
  2. Zmiany z examples/companion_radio (stara wersja → nowa) nakładane 3-stronnie (git merge-file) na companion_mapper/
     — nasze poprawki zostają, oficjalne zmiany wchodzą. Nowe/usunięte pliki upstream w katalogu głównym i ui-new/ też.
  3. MESHCORE_REF w build.yml i MAPPER_FW_VERSION (+1 w ostatniej cyfrze) podbite.
Wynik w GITHUB_OUTPUT: status = same | ok | conflict, tag, version, conflicts.
Konflikty: pliki zostają nietknięte, workflow zakłada zgłoszenie (issue) z listą do ręcznego scalenia.
"""
import os, re, subprocess, sys, tempfile, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = ROOT / ".github/workflows/build.yml"
CM = ROOT / "companion_mapper"
UPSTREAM = "https://github.com/meshcore-dev/MeshCore"
SUBDIRS = ["", "ui-new"]          # katalogi companion_radio, które mamy u siebie (ui-sf6 jest tylko nasze)


def sh(*a, cwd=None, check=True):
    r = subprocess.run(a, cwd=cwd, capture_output=True, text=True)
    if check and r.returncode != 0:
        sys.exit(f"{' '.join(a)}\n{r.stdout}\n{r.stderr}")
    return r


def out(**kv):
    with open(os.environ.get("GITHUB_OUTPUT", "/dev/null"), "a") as f:
        for k, v in kv.items():
            f.write(f"{k}={v}\n")
    print(kv)


def vkey(t):
    return tuple(int(x) for x in t.split("-v")[1].split("."))


build = BUILD.read_text()
m = re.search(r"MESHCORE_REF:\s*([0-9a-f]{40})\s*#\s*(companion-v[\d.]+)", build)
if not m:
    sys.exit("build.yml: brak 'MESHCORE_REF: <sha>  # companion-vX.Y.Z'")
cur_sha, cur_tag = m.group(1), m.group(2)

tags = {}
for line in sh("git", "ls-remote", "--tags", UPSTREAM).stdout.splitlines():
    sha, ref = line.split()
    mt = re.fullmatch(r"refs/tags/(companion-v\d+\.\d+\.\d+)(\^\{\})?", ref)
    if mt:
        if mt.group(2) or mt.group(1) not in tags:
            tags[mt.group(1)] = sha          # ^{} = commit pod tagiem z adnotacją
new_tag = max(tags, key=vkey)
if vkey(new_tag) <= vkey(cur_tag):
    out(status="same", tag=cur_tag)
    sys.exit(0)
new_sha = tags[new_tag]
print(f"MeshCore: {cur_tag} ({cur_sha[:7]}) -> {new_tag} ({new_sha[:7]})")

up = pathlib.Path(tempfile.mkdtemp()) / "mc"
sh("git", "init", "-q", str(up))
sh("git", "remote", "add", "o", UPSTREAM, cwd=up)
sh("git", "fetch", "-q", "--filter=blob:none", "o", cur_sha, new_sha, cwd=up)


def ls(sha, sub):
    p = "examples/companion_radio/" + (sub + "/" if sub else "")
    r = sh("git", "ls-tree", "--name-only", sha, p, cwd=up)
    return {pathlib.PurePosixPath(x).name for x in r.stdout.split() if "." in pathlib.PurePosixPath(x).name}


def show(sha, path):
    r = sh("git", "show", f"{sha}:examples/companion_radio/{path}", cwd=up, check=False)
    return r.stdout if r.returncode == 0 else None


conflicts, changed = [], []
tmp = pathlib.Path(tempfile.mkdtemp())
for sub in SUBDIRS:
    old, new = ls(cur_sha, sub), ls(new_sha, sub)
    for name in sorted(old | new):
        rel = f"{sub}/{name}" if sub else name
        ours = CM / rel
        base, theirs = show(cur_sha, rel), show(new_sha, rel)
        if base == theirs:
            continue
        if base is None:                                   # nowy plik upstream
            if not ours.exists():
                ours.parent.mkdir(parents=True, exist_ok=True)
                ours.write_text(theirs); changed.append(rel + " (nowy)")
            continue
        if theirs is None:                                 # usunięty upstream
            if ours.exists():
                if ours.read_text() == base:
                    ours.unlink(); changed.append(rel + " (usunięty)")
                else:
                    conflicts.append(rel + " (usunięty upstream, u nas zmieniony)")
            continue
        if not ours.exists():
            continue                                       # tego pliku celowo nie mamy
        (tmp / "b").write_text(base); (tmp / "t").write_text(theirs)
        merged = tmp / "m"; merged.write_text(ours.read_text())
        r = sh("git", "merge-file", "-L", "mapper", "-L", "base", "-L", "upstream",
               str(merged), str(tmp / "b"), str(tmp / "t"), check=False)
        if r.returncode != 0:
            conflicts.append(f"{rel} ({r.returncode} konflikt.)")
        else:
            ours.write_text(merged.read_text()); changed.append(rel)

if conflicts:
    sh("git", "checkout", "--", "companion_mapper", cwd=ROOT)
    sh("git", "clean", "-fdq", "companion_mapper", cwd=ROOT)
    out(status="conflict", tag=new_tag, conflicts="; ".join(conflicts))
    sys.exit(0)

# MESHCORE_REF i wersja firmware
BUILD.write_text(build.replace(m.group(0), m.group(0).replace(cur_sha, new_sha).replace(cur_tag, new_tag)))
ver = None
for p in (ROOT / "sf6_mapper/MapperProtocol.h", CM / "MapperProtocol.h"):
    t = p.read_text()
    mv = re.search(r'(#define MAPPER_FW_VERSION\s+")(\d+)\.(\d+)\.(\d+)"', t)
    ver = f"{mv.group(2)}.{mv.group(3)}.{int(mv.group(4)) + 1}"
    p.write_text(t.replace(mv.group(0), f'{mv.group(1)}{ver}"'))
print("Zmienione:", ", ".join(changed) or "(tylko wersja MeshCore)")
out(status="ok", tag=new_tag, version=ver)

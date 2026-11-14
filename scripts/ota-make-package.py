#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ota-make-package: an omonullOS update (stage 1, user space) from a built root.

    ota-make-package.py --root ROOT --version 0.2 --tag beyondx-v0.2 \
        --notes notes.txt --out OUT [--commit SHA] [--overlay DIR ...] \
        [--base PREVIOUS/manifest.json]

ROOT is a built root filesystem (scripts/build-beyondx-image.sh's, mounted or
unpacked). Taken from it:
  - everything below usr/local that the updater may write (its ALLOWED list,
    read from rootfs/overlay-beyondx/usr/local/sbin/omonullos-update itself, so
    the two can never disagree), minus its DENIED list;
  - from etc/ only the files the overlays (--overlay, default the three this
    tree has) put there: the rest of /etc is Arch's, pacman's business.
OUT gets:
  update-VERSION.tar.zst   the files, paths relative to /
  update-BASE-to-VERSION.tar.zst   with --base: only what changed since the
                           previous release (the phone takes this one when it
                           has BASE installed); files gone since go to "remove"
  manifest.json            what the phone checks; sign it with zota-sign
  installed.json           the same manifest, for the image's
                           /var/lib/omonullos-update/ (a fresh image then knows
                           its version and which /etc files are still ours)

Nothing here signs: the key is only with the owner.

Added for OmonullOS, November 2026.
"""
import argparse
import hashlib
import importlib.machinery
import importlib.util
import json
import os
import subprocess
import sys
import tarfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.dirname(HERE)
UPDATER = os.path.join(TREE, "rootfs/overlay-beyondx/usr/local/sbin/omonullos-update")
OVERLAYS = ["os/overlay", "rootfs/overlay", "rootfs/overlay-beyondx"]
SKIP_NAMES = ("__pycache__",)
SKIP_SUFFIX = (".pyc", ".bak", ".orig", ".omonullos-new")


def load_updater():
    loader = importlib.machinery.SourceFileLoader("omonullos_update", UPDATER)
    spec = importlib.util.spec_from_loader("omonullos_update", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def usable(upd, rel, compat):
    if compat == 1 and not rel.startswith(upd.ALLOWED_V1):
        return False
    try:
        upd.check_path(rel)
        return True
    except ValueError:
        return False


def collect(upd, root, overlays, compat):
    rels = set()
    for prefix in (upd.ALLOWED_V1 if compat == 1 else upd.ALLOWED):
        if prefix == "etc/":
            continue
        base = os.path.join(root, prefix)
        for d, dirs, files in os.walk(base):
            dirs[:] = sorted(x for x in dirs if x not in SKIP_NAMES)
            for name in files + [x for x in dirs if os.path.islink(os.path.join(d, x))]:
                rels.add(os.path.relpath(os.path.join(d, name), root))
    for ov in overlays:
        base = os.path.join(ov, "etc")
        for d, dirs, files in os.walk(base):
            for name in files + [x for x in dirs if os.path.islink(os.path.join(d, x))]:
                rel = os.path.relpath(os.path.join(d, name), ov)
                if os.path.lexists(os.path.join(root, rel)):
                    rels.add(rel)
    out = []
    for rel in sorted(rels):
        if rel.endswith(SKIP_SUFFIX) or not usable(upd, rel, compat):
            continue
        p = os.path.join(root, rel)
        if os.path.islink(p):
            target = os.readlink(p)
            if os.path.basename(target) == "busybox-zboot":
                continue  # made by the kernel job, not a stage-1 file
            if os.path.isabs(target):
                # updaters extract with tarfile's data filter: no absolute links
                sys.exit(f"{rel}: an absolute symlink ({target}); make it relative")
            out.append({"path": rel, "type": "link", "target": target})
        elif os.path.isfile(p):
            st = os.stat(p)
            out.append({"path": rel, "sha256": sha256(p), "size": st.st_size,
                        "mode": "%o" % (st.st_mode & 0o7777)})
    return out


def pack(root, files, out):
    """A tar.zst of these entries; fixed owner, times and order, so the same
    root makes the same package."""
    tar = out[:-len(".zst")] if out.endswith(".zst") else out + ".tar"
    with tarfile.open(tar, "w", format=tarfile.PAX_FORMAT) as t:
        for e in files:
            p = os.path.join(root, e["path"])
            ti = t.gettarinfo(p, arcname=e["path"])
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = "root"
            ti.mtime = 0
            if ti.isfile():
                with open(p, "rb") as f:
                    t.addfile(ti, f)
            else:
                t.addfile(ti)
    # the zstd tool, not tarfile's: Python before 3.14 (GitHub's runners) has none
    subprocess.run(["zstd", "-q", "-f", "-12", "-T0", "--rm", tar, "-o", out], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--notes", required=True, help="Uzbek «what is new», a text file")
    ap.add_argument("--out", required=True)
    ap.add_argument("--commit", default="")
    ap.add_argument("--after", default="reboot", choices=["none", "restart-shell", "reboot"])
    ap.add_argument("--overlay", action="append", help="overlay dirs whose etc/ files are ours")
    ap.add_argument("--base", help="the previous release's manifest.json: also make a delta from it")
    ap.add_argument("--compat", type=int, default=0,
                    help="1: only what updater 1 may install (a bridge release: no modules, no zboot)")
    ap.add_argument("--kernel-config", help="the kernel's .config (needed when modules are in)")
    ap.add_argument("--kernel-files", default="",
                    help="space-separated globs: only these module files go in (the rest of the root's "
                         "modules stay as the phone has them)")
    a = ap.parse_args()

    upd = load_updater()
    overlays = a.overlay or [os.path.join(TREE, o) for o in OVERLAYS]
    files = collect(upd, a.root, overlays, a.compat)
    if a.kernel_files:
        import fnmatch
        pats = a.kernel_files.split()
        files = [e for e in files if not upd.kernel_side(e["path"])
                 or any(fnmatch.fnmatch(e["path"], p) for p in pats)]
    if not files:
        sys.exit("no files found under " + a.root)
    os.makedirs(a.out, exist_ok=True)

    name = f"update-{a.version}.tar.zst"
    pkg = os.path.join(a.out, name)
    pack(a.root, files, pkg)
    delta, remove = None, []
    if a.base:
        with open(a.base) as f:
            base = json.load(f)
        old = {e["path"]: (e.get("type", "file"), e.get("sha256"), e.get("target"), e.get("mode"))
               for e in base["files"]}
        changed = [e for e in files
                   if old.get(e["path"]) != (e.get("type", "file"), e.get("sha256"), e.get("target"), e.get("mode"))]
        now = {e["path"] for e in files}
        remove = sorted(p for p in old if p not in now)
        dname = f"update-{base['version']}-to-{a.version}.tar.zst"
        dpkg = os.path.join(a.out, dname)
        pack(a.root, changed, dpkg)
        delta = {"base": base["version"], "name": dname, "size": os.path.getsize(dpkg), "sha256": sha256(dpkg)}
        print(f"delta from {base['version']}: {len(changed)} changed, {len(remove)} removed, "
              f"{delta['size'] >> 10} KB")
    with open(a.notes, encoding="utf-8") as f:
        notes = f.read().strip()
    kernel, min_updater = None, 1
    kfiles = [e for e in files if upd.kernel_side(e["path"])]
    if kfiles or any(e["path"].startswith("usr/local/lib/zboot/") for e in files):
        min_updater = 2
    if kfiles:
        if not a.kernel_config:
            sys.exit(f"{len(kfiles)} module files in, but no --kernel-config")
        with open(a.kernel_config, "rb") as f:
            conf_digest = upd.config_digest(f.read())
        rels = sorted({e["path"].split("/")[3] for e in kfiles if e["path"].startswith("usr/lib/modules/")})
        if len(rels) > 1:
            sys.exit(f"modules for several releases: {rels}")
        magics = set()
        for e in kfiles:
            if e["path"].endswith(".ko"):
                out = subprocess.run(["modinfo", "-F", "vermagic", os.path.join(a.root, e["path"])],
                                     stdout=subprocess.PIPE, text=True).stdout.strip()
                magics.add(out)
        if len(magics) != 1 or "" in magics:
            sys.exit(f"modules with different (or no) vermagic: {sorted(magics)}")
        vermagic = magics.pop()
        release = rels[0] if rels else vermagic.split()[0]
        if not vermagic.startswith(release + " "):
            sys.exit(f"vermagic {vermagic!r} is not for {release}")
        kernel = {"release": release, "vermagic": vermagic, "config_digest": conf_digest}
    manifest = {
        "format": 1,
        "device": upd.DEVICE,
        "version": a.version,
        "tag": a.tag,
        "commit": a.commit,
        "date": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "min_updater": min_updater,
        "after": a.after,
        "notes": notes,
        "package": {"name": name, "size": os.path.getsize(pkg), "sha256": sha256(pkg)},
        "files": files,
        "remove": remove,
    }
    if delta:
        manifest["delta"] = delta
    if kernel:
        manifest["kernel"] = kernel
    upd.check_manifest(manifest)
    data = json.dumps(manifest, ensure_ascii=False, indent=1).encode() + b"\n"
    with open(os.path.join(a.out, "manifest.json"), "wb") as f:
        f.write(data)
    inst = dict(manifest, installed_at=0)
    with open(os.path.join(a.out, "installed.json"), "w", encoding="utf-8") as f:
        json.dump(inst, f, ensure_ascii=False, indent=1)
    n_bytes = sum(e.get("size", 0) for e in files)
    print(f"{len(files)} files, {n_bytes >> 20} MB, package {manifest['package']['size'] >> 20} MB -> {a.out}")


if __name__ == "__main__":
    main()

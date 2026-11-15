#!/bin/sh
# ota-selftest: omonullos-update end to end in a scratch directory, as any
# user, touching nothing of the system: a throwaway key, two fake releases
# (v0.1 -> v0.2), a fake phone root; check, apply, the /etc rule, a bad
# signature, a tampered package, a path escape, rollback.
#
#   scripts/ota-selftest.sh [WORKDIR]
#
# Added for OmonullOS, November 2026.
set -eu
TREE=$(cd "$(dirname "$0")/.." && pwd)
UPD=$TREE/rootfs/overlay-beyondx/usr/local/sbin/omonullos-update
MK=$TREE/scripts/ota-make-package.py
W=${1:-$(mktemp -d)}
rm -rf "$W"; mkdir -p "$W"
cd "$W"
pass=0
ok() { pass=$((pass + 1)); echo "ok $pass - $*"; }
fail() { echo "FAIL - $*"; exit 1; }

openssl genpkey -algorithm ed25519 -out key.pem 2>/dev/null
openssl pkey -in key.pem -pubout -out pub.pem
openssl genpkey -algorithm ed25519 -out other.pem 2>/dev/null

# a built root for each version, and the overlay that owns etc/
mkroot() { # DIR VER
	mkdir -p "$1/usr/local/bin" "$1/usr/local/share/ztos" "$1/etc/zt" "$1/usr/local/lib/ztos-sdk/bin"
	printf '#!/bin/sh\necho ztmetro %s\n' "$2" > "$1/usr/local/bin/ztmetro"
	chmod 755 "$1/usr/local/bin/ztmetro"
	echo "theme $2" > "$1/usr/local/share/ztos/theme"
	echo "conf $2" > "$1/etc/zt/a.conf"
	echo "conf b $2" > "$1/etc/zt/b.conf"
	echo sdk > "$1/usr/local/lib/ztos-sdk/bin/ztos-sdk"
	ln -sf ../lib/ztos-sdk/bin/ztos-sdk "$1/usr/local/bin/ztos-sdk"
	echo arch > "$1/etc/arch-owned"         # not in the overlay: never taken
	mkdir -p "$1/usr/lib/modules"; echo ko > "$1/usr/lib/modules/x.ko"
}
mkroot r1 0.1; mkroot r2 0.2
echo new-in-02 > r2/usr/local/bin/znew
echo gone-in-02 > r1/usr/local/bin/zold
ln -s znew r1/usr/local/bin/zold-link    # a link gone in 0.2 (0.2.2's wants link)
mkdir -p ov/etc/zt; : > ov/etc/zt/a.conf; : > ov/etc/zt/b.conf
echo "Sinov versiyasi" > notes

mkrel() { # ROOT VER [BASEVER]
	python3 "$MK" --root "$1" --version "$2" --tag "beyondx-v$2" --notes notes --out "out$2" --overlay ov \
		${3:+--base "out$3/manifest.json"} >/dev/null
	mkdir -p "rel/beyondx-v$2"
	cp "out$2/manifest.json" "out$2"/update-*.tar.zst "rel/beyondx-v$2/"
	openssl pkeyutl -sign -inkey key.pem -rawin -in "rel/beyondx-v$2/manifest.json" \
		-out "rel/beyondx-v$2/manifest.json.sig"
}
mkrel r1 0.1

grep -q arch-owned out0.1/manifest.json && fail "an Arch /etc file got in"
grep -q modules out0.1/manifest.json && fail "modules got in"
ok "package: only usr/local and overlay etc"

# the phone: v0.1 files, the key, the updater's state dirs
mkdir -p phone/usr/local/share/omonullos/ota-keys
cp pub.pem phone/usr/local/share/omonullos/ota-keys/owner.pem
tar -C phone --zstd -xf out0.1/update-0.1.tar.zst
mkdir -p phone/var/lib/omonullos-update
cp out0.1/installed.json phone/var/lib/omonullos-update/
export OMONULLOS_UPDATE_ROOT="$W/phone" OMONULLOS_UPDATE_RUN="$W/run" \
	OMONULLOS_UPDATE_VAR="$W/phone/var/lib/omonullos-update" OMONULLOS_UPDATE_LOG="$W/log" \
	OMONULLOS_UPDATE_CONF="$W/conf"
echo "source=dir:$W/rel" > conf
st() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["state"])' run/status.json; }

python3 "$UPD" check --quiet; [ "$(st)" = uptodate ] || fail "v0.1 installed: expected uptodate, got $(st)"
ok "uptodate when nothing newer"

# the owner changed a.conf by hand; b.conf is still ours
echo "mine" > phone/etc/zt/a.conf
mkrel r2 0.2 0.1
grep -q '"base": "0.1"' rel/beyondx-v0.2/manifest.json || fail "no delta in the 0.2 manifest"
tar --zstd -tf rel/beyondx-v0.2/update-0.1-to-0.2.tar.zst | grep -q ztos-sdk && fail "an unchanged file is in the delta"
python3 "$UPD" check --quiet; [ "$(st)" = available ] || fail "expected available, got $(st)"
ok "v0.2 found"

python3 "$UPD" apply || fail "apply: $(cat run/status.json)"
[ "$(st)" = done ] || fail "apply state $(st)"
[ "$(phone/usr/local/bin/ztmetro)" = "ztmetro 0.2" ] || fail "ztmetro not updated"
[ -x phone/usr/local/bin/ztmetro ] || fail "mode lost"
[ "$(cat phone/usr/local/bin/znew)" = new-in-02 ] || fail "new file missing"
[ "$(cat phone/etc/zt/a.conf)" = mine ] || fail "the owner's a.conf was overwritten"
[ "$(cat phone/etc/zt/a.conf.new)" = "conf 0.2" ] || fail "no a.conf.new"
[ "$(cat phone/etc/zt/b.conf)" = "conf b 0.2" ] || fail "b.conf (unchanged, ours) not updated"
[ "$(readlink phone/usr/local/bin/ztos-sdk)" = ../lib/ztos-sdk/bin/ztos-sdk ] || fail "link"
[ ! -e phone/usr/local/bin/zold ] || fail "a file gone in 0.2 is still there"
[ ! -L phone/usr/local/bin/zold-link ] || fail "a link gone in 0.2 is still there"
grep -q "apply: package update-0.1-to-0.2.tar.zst" log || fail "the delta was not used"
ok "apply (delta): files, modes, new file, link, removed file; owner's conf kept (.new), ours replaced"

python3 "$UPD" rollback || fail "rollback: $(cat run/status.json)"
[ "$(phone/usr/local/bin/ztmetro)" = "ztmetro 0.1" ] || fail "rollback ztmetro"
[ ! -e phone/usr/local/bin/znew ] || fail "rollback left a created file"
[ "$(cat phone/usr/local/bin/zold)" = gone-in-02 ] || fail "rollback did not bring back a removed file"
[ "$(readlink phone/usr/local/bin/zold-link)" = znew ] || fail "rollback did not bring back a removed link"
[ ! -e phone/etc/zt/a.conf.new ] || fail "rollback left a.conf.new"
[ "$(cat phone/etc/zt/b.conf)" = "conf b 0.1" ] || fail "rollback b.conf"
[ "$(cat phone/etc/zt/a.conf)" = mine ] || fail "rollback touched the owner's conf"
grep -q '"version": "0.1"' phone/var/lib/omonullos-update/installed.json || fail "rollback version"
ok "rollback: back to 0.1, created files gone"

# a signature by some other key: refused, nothing offered
cp rel/beyondx-v0.2/manifest.json.sig good.sig
openssl pkeyutl -sign -inkey other.pem -rawin -in rel/beyondx-v0.2/manifest.json -out rel/beyondx-v0.2/manifest.json.sig
python3 "$UPD" check --quiet; [ "$(st)" = badsig ] || fail "other key: expected badsig, got $(st)"
ok "a signature by another key is refused"
cp good.sig rel/beyondx-v0.2/manifest.json.sig

# the package swapped after signing: sha256 refuses it, nothing changes
python3 "$UPD" check --quiet
cp rel/beyondx-v0.2/update-0.1-to-0.2.tar.zst good.tar.zst
mkdir evil; tar -C evil --zstd -xf good.tar.zst; echo evil > evil/usr/local/bin/ztmetro
tar -C evil --zstd -cf rel/beyondx-v0.2/update-0.1-to-0.2.tar.zst .
python3 "$UPD" apply && fail "a tampered package was installed"
[ "$(phone/usr/local/bin/ztmetro)" = "ztmetro 0.1" ] || fail "tampered package changed files"
ok "a tampered package is refused, nothing changed"
cp good.tar.zst rel/beyondx-v0.2/update-0.1-to-0.2.tar.zst

# a manifest that tries to write outside (signed by the right key even)
mkdir -p rel/beyondx-v0.3
python3 - "$W/rel/beyondx-v0.2/manifest.json" "$W/rel/beyondx-v0.3/manifest.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
m["version"] = "0.3"
m["files"].append({"path": "usr/lib/modules/x.ko", "sha256": "0" * 64, "mode": "644"})
open(sys.argv[2], "w").write(json.dumps(m))
EOF
cp rel/beyondx-v0.2/update-0.2.tar.zst rel/beyondx-v0.3/
openssl pkeyutl -sign -inkey key.pem -rawin -in rel/beyondx-v0.3/manifest.json -out rel/beyondx-v0.3/manifest.json.sig
python3 "$UPD" check --quiet; [ "$(st)" = available ] || fail "expected v0.2 offered past the refused v0.3, got $(st)"
grep -q '"version": "0.2"' phone/var/lib/omonullos-update/available.json || fail "not v0.2"
grep -q "manifest refused: modules only below usr/lib/modules/RELEASE/extra" log || fail "v0.3 not refused for the module path"
ok "a signed manifest with a module outside the module places is refused"
rm -rf rel/beyondx-v0.3

# a release that needs a newer updater (stage 2: BOOT, modules) is only reported
python3 - "$W/rel/beyondx-v0.2/manifest.json" "$W/rel/beyondx-v0.4/manifest.json" <<'EOF'
import json, os, sys
m = json.load(open(sys.argv[1]))
m["version"] = "0.4"; m["min_updater"] = 3; m["boot"] = {"name": "boot.img", "sha256": "0" * 64}
os.makedirs(os.path.dirname(sys.argv[2]), exist_ok=True)
open(sys.argv[2], "w").write(json.dumps(m))
EOF
openssl pkeyutl -sign -inkey key.pem -rawin -in rel/beyondx-v0.4/manifest.json -out rel/beyondx-v0.4/manifest.json.sig
python3 "$UPD" check --quiet; [ "$(st)" = needs-updater ] || fail "expected needs-updater, got $(st)"
python3 "$UPD" apply && fail "a min_updater 3 release was applied"
[ "$(phone/usr/local/bin/ztmetro)" = "ztmetro 0.1" ] || fail "needs-updater changed files"
ok "a release for a newer updater (BOOT) is refused, nothing changed"
rm -rf rel/beyondx-v0.4

# and the real upgrade once more, then a second update on top (0.2 -> 0.2.1)
python3 "$UPD" check --quiet && python3 "$UPD" apply || fail "re-apply 0.2"
cp -a r2 r3; printf '#!/bin/sh\necho ztmetro 0.2.1\n' > r3/usr/local/bin/ztmetro
mkrel r3 0.2.1 0.2
python3 "$UPD" check --quiet && python3 "$UPD" apply || fail "apply 0.2.1"
[ "$(phone/usr/local/bin/ztmetro)" = "ztmetro 0.2.1" ] || fail "0.2.1"
python3 "$UPD" rollback || fail "rollback 0.2.1"
[ "$(phone/usr/local/bin/ztmetro)" = "ztmetro 0.2" ] || fail "rollback to 0.2"
ok "0.1 -> 0.2 -> 0.2.1 -> rollback -> 0.2"

# something fails half way (a directory where a file goes, after ztmetro is
# already replaced): what was changed is put back by itself
python3 "$UPD" rollback >/dev/null 2>&1      # 0.2 -> 0.1
rm -f rel/beyondx-v0.2.1/update-0.2-to-0.2.1.tar.zst   # only the full package from 0.1
mv phone/usr/local/share/ztos/theme theme.saved; mkdir phone/usr/local/share/ztos/theme
python3 "$UPD" check --quiet
python3 "$UPD" apply 2>/dev/null && fail "apply went through a directory in the way"
[ "$(phone/usr/local/bin/ztmetro)" = "ztmetro 0.1" ] || fail "half-way failure left ztmetro $(phone/usr/local/bin/ztmetro)"
[ ! -e phone/usr/local/bin/znew ] || fail "half-way failure left a created file"
grep -q '"version": "0.1"' phone/var/lib/omonullos-update/installed.json || fail "installed.json not 0.1"
grep -q "failed half way" log || fail "no undo in the log"
ok "a failure half way puts everything back by itself"
rmdir phone/usr/local/share/ztos/theme; mv theme.saved phone/usr/local/share/ztos/theme

# a directory on the phone replaced by a symlink to outside: never written through
mkdir outside
mv phone/usr/local/share/ztos phone/usr/local/share/ztos.real
ln -s "$W/outside" phone/usr/local/share/ztos
python3 "$UPD" check --quiet
python3 "$UPD" apply 2>/dev/null && fail "wrote through a symlinked directory"
[ -z "$(ls outside)" ] || fail "a file landed outside the root"
grep -q "symlink on the way" log || fail "no symlink refusal in the log"
[ "$(phone/usr/local/bin/ztmetro)" = "ztmetro 0.1" ] || fail "symlink refusal came after files were written"
ok "a symlinked directory on the way is refused, nothing written outside"
rm phone/usr/local/share/ztos; mv phone/usr/local/share/ztos.real phone/usr/local/share/ztos

# ---- updater 2: our kernel modules, only for the running kernel
printf 'license=GPL\0vermagic=7.1.0-zboot SMP preempt mod_unload aarch64\0' > mi
mkko() { mkdir -p "$(dirname "$1")"; objcopy -I binary -O elf64-littleaarch64 --rename-section .data=.modinfo mi "$1"; }
echo "CONFIG_TEST=y" > kconf
export OMONULLOS_UPDATE_UNAME_R=7.1.0-zboot OMONULLOS_UPDATE_KCONFIG="$W/kconf"
signrel() { # VER (out$VER -> rel/beyondx-v$VER, signed)
	mkdir -p "rel/beyondx-v$1"
	cp "out$1/manifest.json" "out$1"/update-*.tar.zst "rel/beyondx-v$1/"
	openssl pkeyutl -sign -inkey key.pem -rawin -in "rel/beyondx-v$1/manifest.json" -out "rel/beyondx-v$1/manifest.json.sig"
}
python3 "$UPD" check --quiet && python3 "$UPD" apply || fail "to 0.2.1"
rm -rf r4; cp -a r3 r4
mkko r4/usr/local/lib/zboot/tools/zsleep.ko
mkko r4/usr/lib/modules/7.1.0-zboot/extra/snd/snd.ko
printf '#!/bin/sh\necho load\n' > r4/usr/local/lib/zboot/tools/zaudio-load.sh
chmod 755 r4/usr/local/lib/zboot/tools/zaudio-load.sh

python3 "$MK" --root r4 --version 0.2.5 --tag beyondx-v0.2.5 --notes notes --out outb --overlay ov --compat 1 >/dev/null
grep -q 'zboot\|modules' outb/manifest.json && fail "a bridge release took zboot or modules"
grep -q '"min_updater": 1' outb/manifest.json || fail "a bridge release is not min_updater 1"
python3 "$MK" --root r4 --version 0.2.5 --tag beyondx-v0.2.5 --notes notes --out outx --overlay ov \
	--kernel-files all 2>/dev/null && fail "modules packed without --kernel-config"
python3 "$MK" --root r4 --version 0.2.5 --tag beyondx-v0.2.5 --notes notes --out outn --overlay ov >/dev/null ||
	fail "no --kernel-files: should pack without modules"
grep -q 'usr/lib/modules\|zsleep.ko' outn/manifest.json && fail "modules went in without --kernel-files"
ok "a bridge release (--compat 1) leaves modules and zboot out; modules only when asked, and need --kernel-config"

python3 "$MK" --root r4 --version 0.3.0 --tag beyondx-v0.3.0 --notes notes --out out0.3.0 --overlay ov \
	--kernel-config kconf --kernel-files all --base out0.2.1/manifest.json >/dev/null
grep -q '"min_updater": 2' out0.3.0/manifest.json || fail "a module release is not min_updater 2"
grep -q '"release": "7.1.0-zboot"' out0.3.0/manifest.json || fail "no kernel in the manifest"
signrel 0.3.0
python3 "$UPD" check --quiet; [ "$(st)" = available ] || fail "module release: expected available, got $(st)"
python3 "$UPD" apply || fail "module apply: $(cat run/status.json)"
[ -f phone/usr/local/lib/zboot/tools/zsleep.ko ] || fail "zsleep.ko not installed"
[ -f phone/usr/lib/modules/7.1.0-zboot/extra/snd/snd.ko ] || fail "snd.ko not installed"
[ -f phone/var/lib/omonullos-update/trial.json ] || fail "no trial after a module update"
ok "a module release for the running kernel installs, the next boots are a trial"

python3 "$UPD" boot; python3 "$UPD" boot
[ -f phone/usr/local/lib/zboot/tools/zsleep.ko ] || fail "rolled back too early"
python3 "$UPD" boot
[ ! -e phone/usr/local/lib/zboot/tools/zsleep.ko ] || fail "the third boot without boot-ok did not roll back"
[ ! -e phone/var/lib/omonullos-update/trial.json ] || fail "trial left after the rollback"
grep -q '"version": "0.2.1"' phone/var/lib/omonullos-update/installed.json || fail "not back on 0.2.1"
ok "two boots that never stayed up: the third puts the old files back"

python3 "$UPD" check --quiet && python3 "$UPD" apply || fail "module apply again"
python3 "$UPD" boot; python3 "$UPD" boot-ok
[ ! -e phone/var/lib/omonullos-update/trial.json ] || fail "boot-ok left the trial"
python3 "$UPD" boot; python3 "$UPD" boot; python3 "$UPD" boot
[ -f phone/usr/local/lib/zboot/tools/zsleep.ko ] || fail "rolled back after boot-ok"
ok "a boot that stayed up (boot-ok) ends the trial"

python3 "$UPD" rollback >/dev/null 2>&1
echo "CONFIG_TEST=m" > kconf2
OMONULLOS_UPDATE_KCONFIG="$W/kconf2" python3 "$UPD" check --quiet
[ "$(st)" = kernel ] || fail "another kernel config: expected kernel, got $(st)"
OMONULLOS_UPDATE_UNAME_R=7.2.0 python3 "$UPD" check --quiet
[ "$(st)" = kernel ] || fail "another kernel release: expected kernel, got $(st)"
grep -q "modules not for this kernel" log || fail "no kernel refusal in the log"
ok "modules for another kernel (config or release) are never offered"

python3 -c '
import json, sys
m = json.load(open(sys.argv[1]))
m["kernel"]["vermagic"] = "7.1.0-zboot SMP preempt aarch64"
open(sys.argv[1], "w").write(json.dumps(m))
' "$W/rel/beyondx-v0.3.0/manifest.json"
openssl pkeyutl -sign -inkey key.pem -rawin -in rel/beyondx-v0.3.0/manifest.json -out rel/beyondx-v0.3.0/manifest.json.sig
python3 "$UPD" check --quiet && python3 "$UPD" apply 2>/dev/null && fail "a module with another vermagic went in"
[ ! -e phone/usr/local/lib/zboot/tools/zsleep.ko ] || fail "vermagic refusal left a module"
grep -q "vermagic of" log || fail "no vermagic refusal in the log"
ok "a module whose vermagic is not the manifest's is refused"

# the next release without modules: what 0.3.0 put stays (not in "remove")
python3 "$MK" --root r4 --version 0.3.1 --tag beyondx-v0.3.1 --notes notes --out out0.3.1 --overlay ov \
	--base out0.3.0/manifest.json >/dev/null || fail "0.3.1 without modules"
python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); sys.exit(1 if m["remove"] else 0)' out0.3.1/manifest.json ||
	fail "a module left out of a release is removed from the phone"
ok "modules left out of a release stay on the phone (not removed)"

echo "all $pass passed ($W)"

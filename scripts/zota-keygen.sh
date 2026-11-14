#!/bin/sh
# zota-keygen: make the key that signs omonullOS updates (once).
#
#   sudo sh scripts/zota-keygen.sh
#
# The private key is made in memory and written only encrypted (PKCS#8,
# AES-256, scrypt), with a passphrase the owner types; it never exists on
# disk in the clear. It stays in /root/omonullos-ota/ (root only); a copy of
# the ENCRYPTED file is left for the owner's own backups (Telegram, a flash
# drive...), to be deleted after. The public half goes into the tree
# (image/beyondx/ota-keys/), from where the image build puts it on phones.
#
# Added for OmonullOS, November 2026.
set -eu
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
TREE=$(cd "$(dirname "$0")/.." && pwd)
NAME=${ZOTA_KEY_NAME:-owner-2026}
DIR=/root/omonullos-ota
KEY=$DIR/$NAME.key.pem
PUB=$TREE/image/beyondx/ota-keys/$NAME.pem
OWNER=$(stat -c %U "$TREE")
BACKUP=$(getent passwd "$OWNER" | cut -d: -f6)/omonullos-ota-$NAME.key.pem

[ -e "$KEY" ] && { echo "already there: $KEY (not made again)" >&2; exit 1; }
umask 077
mkdir -p "$DIR"; chmod 700 "$DIR"

cat <<'EOF'

  omonullOS yangilanişlari uçun imzo kaliti yaratiladi.

  Parol (passphrase) yozasiz, ikki marta. Maslahat:
   - kamida 4 ta söz yoki 14+ belgi;
   - parolni qoğozga ham yozib qöying: unutilsa, kalit işlamaydi va
     yangilaniş çiqarib bölmaydi;
   - parol ekranda körinmaydi, bu normal.

EOF
openssl genpkey -algorithm ed25519 | openssl pkcs8 -topk8 -v2 aes-256-cbc -scrypt -out "$KEY"
chmod 600 "$KEY"

echo
echo "  Tekşiruv: parolni yana bir marta yozing (açiq kalit çiqariladi)."
mkdir -p "$(dirname "$PUB")"
openssl pkey -in "$KEY" -pubout -out "$PUB.tmp"
mv "$PUB.tmp" "$PUB"
chown "$OWNER:" "$PUB" "$(dirname "$PUB")"
chmod 644 "$PUB"

# the encrypted file must not open without the passphrase
if openssl pkey -in "$KEY" -passin pass: -noout 2>/dev/null; then
	echo "XATO: kalit parolsiz oçildi, shifrlanmagan!" >&2
	rm -f "$KEY" "$PUB"
	exit 1
fi

echo
echo "  Oxirgi tekşiruv: imzo qöyib köramiz, parolni yana bir marta yozing."
t=$(mktemp -d)
echo "omonullOS test" > "$t/m"
openssl pkeyutl -sign -inkey "$KEY" -rawin -in "$t/m" -out "$t/s"
openssl pkeyutl -verify -pubin -inkey "$PUB" -rawin -in "$t/m" -sigfile "$t/s" >/dev/null
rm -rf "$t"

install -m600 -o "$OWNER" "$KEY" "$BACKUP"
echo
echo "  TAYYOR."
echo "  Kalit (shifrlangan):  $KEY"
echo "  Açiq kalit (repoga):  $PUB"
echo "  Zaxira uçun nusxa:    $BACKUP"
echo "  (Claudega «tayyor» deng: nusxani Telegramga yuboradi va öçiradi.)"
echo

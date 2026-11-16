#!/usr/bin/env python3
# mk-patch-h.py: the S10 5G's sensor hub patch as a C header for bbd.c.
#
# The patch is Broadcom firmware shipped inside Samsung's (GPL) kernel as
# drivers/sensorhub/brcm/bbdpl/r_os/bbd_patch_file_beyond.h. It is not kept
# in git; this rebuilds the header from the phone itself: the TWRP kernel on
# RECOVERY carries the same text (three patches: Samsung's "current", then
# "new_old" and "old"; beyondx with hw_rev >= 4 takes "current").
#
#   mk-patch-h.py IMAGE OUT.h      IMAGE: a copy of the recovery partition,
#                                  or a file holding the patch text itself
import re, sys

data = open(sys.argv[1], 'rb').read()
# Crc=236 BlobLength=445094: Samsung's r_os/bbd_patch_file_beyond.h
want = b'<FormatVersion=0x00010003>\n<Crc=236>\n<BlobLength=445094>\n<AsicVersion=0x004775B0>'
start = data.find(want)
if start < 0:
	sys.exit('the beyondx patch (Crc 236, BlobLength 445094) is not in %s' % sys.argv[1])
end = data.find(b'\0', start)
text = data[start:end if end > 0 else len(data)]
if not text.endswith(b'>\n') or not re.fullmatch(rb'[ -~\n]+', text):
	sys.exit('patch text does not look whole')
blob = re.search(rb'<Blob=([0-9A-F\n]+)>', text)
if not blob or len(blob.group(1).replace(b'\n', b'')) != 2 * 445094:
	sys.exit('blob length does not match BlobLength')
with open(sys.argv[2], 'w') as f:
	for line in text.decode().split('\n')[:-1]:
		f.write('"%s\\n" \n' % line)
print('%s: %d bytes of patch text' % (sys.argv[2], len(text)))

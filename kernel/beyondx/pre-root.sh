# Galaxy S10 5G: bring the internal UFS up before initramfs looks for the
# root (ZR_ARCH on userdata). Sourced by initramfs/init as /etc/zboot/pre-root;
# scripts/build-boot.sh --device beyondx --ufs --root puts it there.
# zufs root: every partition read-only but the ZR_ARCH one, and that one only
# if it is userdata.
#
# Added for OmonullOS, October 2026.
log "ufs: zufs root"
/bin/zufs root > /run/zufs.log 2>&1
log "ufs: $(tail -n 1 /run/zufs.log)"

#!/bin/sh
# read_cmdline.sh - show spoof-relevant androidboot.* fields from /proc/cmdline
# Handy for checking whether the SUSFS cmdline spoof is active.
# Usage:  sh read_cmdline.sh

CMD="$(cat /proc/cmdline 2>/dev/null)" || { echo "cannot read /proc/cmdline"; exit 1; }
[ -z "$CMD" ] && { echo "empty /proc/cmdline"; exit 1; }

for f in verifiedbootstate flash.locked vbmeta.device_state veritymode \
         warranty_bit secureboot selinux fuse_state; do
    v="$(printf '%s\n' "$CMD" | tr ' ' '\n' | sed -n "s/^androidboot\.$f=//p" | head -n 1)"
    printf 'androidboot.%-20s %s\n' "$f" "${v:-(absent)}"
done

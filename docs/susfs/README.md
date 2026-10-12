# SUSFS command-line spoofing

Fleux wires the SUSFS `SPOOF_CMDLINE_OR_BOOTCONFIG` read path, so readers of
`/proc/cmdline` can be shown a fake line.

> This affects only processes that read `/proc/cmdline`. Android's `ro.boot.*`
> properties are set by `init` at boot and are **not** changed. Linux 4.19 has
> no `/proc/bootconfig`.

## Files

- `fake_cmdline.example.txt` — sanitized example; contains **no** serial /
  chipid / cpuid.
- `read_cmdline.sh` — prints the spoof-relevant `androidboot.*` fields.

## Usage

1. Copy the example somewhere on your device and edit it, keeping it a **single
   line** without device identifiers, e.g.:

   ```sh
   cp fake_cmdline.example.txt /sdcard/fake_cmdline.txt
   ```

2. In your root manager open **BakaSU → SUSFS → Cmdline / Bootconfig** and enter
   the path to your file. It applies immediately and is re-applied on boot
   (after `/sdcard` is mounted).

3. Verify:

   ```sh
   sh read_cmdline.sh
   ```

   Expected: `verifiedbootstate green`, `flash.locked 1`,
   `vbmeta.device_state locked`, `veritymode enforcing`, … (the rest of your
   real line is preserved).

4. Remove it by clearing the field in the manager.

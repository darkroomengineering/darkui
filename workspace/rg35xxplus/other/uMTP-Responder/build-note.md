# uMTP-Responder vendoring note

- Upstream: https://github.com/viveris/uMTP-Responder
- Tag: `umtprd-1.8.1`
- Commit SHA: `8f87f7b19034055e2665eaaf07f84a41aa5f25e0`
- Vendored as plain files (`.git` and `.github` removed; `LICENSE` kept as-is, GPLv3).

## Build

Target platform binaries for `rg35xxplus` in this repo are all 32-bit ARM
(armhf/gnueabihf) — the device kernel is aarch64 but the darkUI/MinUI
userland it chainloads from, and every other native tool in this tree
(`show.elf`, `dtc`, `fbset`, `init.elf`, `libmsettings.so`), targets
`arm-buildroot-linux-gnueabihf` via 32-bit compat. Built to match, using the
toolchain container's own `CROSS_COMPILE`.

```
cd /Users/frz/Developer/@darkroom/darkui
docker run --rm --platform linux/amd64 -v "$(pwd)/workspace":/root/workspace rg35xxplus-toolchain /bin/bash -c \
  '. ~/.bashrc && cd /root/workspace/rg35xxplus/other/uMTP-Responder && \
   make CC=${CROSS_COMPILE}gcc LDFLAGS="-static -lpthread -lrt -s" OLD_FUNCTIONFS_DESCRIPTORS=1; \
   echo EXIT_CODE=$?'
```

Result: `EXIT_CODE=0`.

```
$ file umtprd
umtprd: ELF 32-bit LSB executable, ARM, EABI5 version 1 (SYSV), statically linked, for GNU/Linux 3.10.0, stripped
```

### Flag notes

- **Static vs dynamic**: tried `LDFLAGS=-static` alone first. On GNU make,
  a command-line `LDFLAGS=...` assignment *overrides* (rather than appends
  to) the Makefile's `LDFLAGS += -lpthread -lrt`, so the first attempt lost
  `-lpthread`/`-lrt` and failed to link (`undefined reference to
  pthread_mutex_lock` etc. across every MTP op file). Fixed by passing the
  full set explicitly: `LDFLAGS="-static -lpthread -lrt -s"` (the trailing
  `-s` restores the Makefile's default strip-on-release behavior, also lost
  the same way). Static link succeeded cleanly on retry — kept static, no
  dynamic fallback needed. The buildroot 2017.11 toolchain's static glibc
  archives resolved every symbol with no `--whole-archive` workaround
  required.

- **`OLD_FUNCTIONFS_DESCRIPTORS=1`**: required. The container's cross
  sysroot ships an older `linux/usb/functionfs.h` that predates
  `FUNCTIONFS_DESCRIPTORS_MAGIC_V2` / `FUNCTIONFS_HAS_FS_DESC` /
  `FUNCTIONFS_HAS_HS_DESC` (upstream's default, non-`OLD_FUNCTIONFS_DESCRIPTORS`
  code path references these unconditionally), so the plain build failed at
  compile time with "undeclared identifier" on `src/usb_gadget.c`. This flag
  is documented upstream for "Kernel version < 3.15" but is really a
  compile-time header-availability switch, not a runtime kernel-version
  gate: the classic single `FUNCTIONFS_DESCRIPTORS_MAGIC` format it emits
  has remained backward-compatible in every FunctionFS kernel driver since,
  so it is safe to use against the on-device 4.9.170 kernel (which is well
  past 3.15 and accepts both formats).

## Config

`umtprd` accepts `-conf <path>` to point at a config file anywhere on disk
(`src/umtprd.c`, `PARAMETER_CONF "-conf"`), so the pak does not need to
install anything to `/etc/umtprd/umtprd.conf` — it points straight at its
own `umtprd.conf` next to the binary.

## Gadget setup ordering (from `conf/umtprd-ffs.sh`, upstream's own
FunctionFS example script)

1. `mkdir` the gadget dir under configfs, `mkdir configs/c.1`,
   `mkdir functions/ffs.mtp`, `mkdir strings/0x409` /
   `configs/c.1/strings/0x409`.
2. Write `idVendor`, `idProduct`, the string descriptors, and
   `configs/c.1/MaxPower`.
3. Symlink `functions/ffs.mtp` into `configs/c.1`.
4. `mkdir /dev/ffs-mtp && mount -t functionfs mtp /dev/ffs-mtp`.
5. Start `umtprd` (it opens the ffs `ep0`/`ep1`/`ep2`/`ep3` endpoints).
6. `sleep 1` to let it finish opening them.
7. Only then `echo <udc-name> > UDC` to enable the gadget.

The pak's `launch.sh` follows this exact order.

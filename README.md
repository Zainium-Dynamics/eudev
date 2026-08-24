# eudev

udev, without systemd. Originally a Gentoo fork of systemd's udev, kept
standalone so device management doesn't require pulling in all of systemd.

This copy is being adapted for **Zainium OS**, which uses its own init
(`quantra`, not systemd) and a non-FHS root layout — everything lives under
`/overlayer/syshub` instead of `/`, `/usr`, `/etc` at the real root. See
[Zainium-Dynamics/quantra-system](https://github.com/Zainium-Dynamics/quantra-system)
for the init side of that.

## What's different from upstream eudev

- Default build prefix is `/overlayer/syshub`, not `/usr`. `sysconfdir`,
  `rootlibdir`, `rootlibexecdir` etc. all follow from that automatically.
  `--prefix=/usr` still gives a normal FHS build if you need one.
- `/run` is untouched — always the real root, never under syshub, since
  it's kernel-mounted tmpfs.
- A few hardcoded fallback paths (rules dirs, hwdb.bin, the helper-program
  search path) got fixed to actually use the configured paths instead of
  `/etc/udev` literals, and `/overlayer/zexlib/union/lib/udev` was added
  as a fallback for rules/helpers installed by `zex` packages.
- See `docs/UDEV_VS_SYSTEMD.md` for what's missing compared to current
  systemd-udev and what of that actually matters here.

## Building

```sh
./autogen.sh   # only needed from a git checkout, not from a release tarball
./configure
make
make check     # runs the test suite against a synthetic sysfs tree, no root needed
```

Defaults to `--prefix=/overlayer/syshub`. For a normal system:

```sh
./configure --prefix=/usr
```

## CI

Every push builds and tests against both prefixes above, so a syshub
change can't quietly break the plain FHS build. Tagged releases
(`vX.Y.Z`) get a tarball built with `make dist` and published
automatically.

## License

GPL-2.0, same as upstream eudev. See `COPYING`.

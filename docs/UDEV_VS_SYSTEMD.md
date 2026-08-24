# eudev vs. systemd's udev — where we've fallen behind

Comparing `eudev-3.2.14` (this tree) against `systemd-261.2`'s `src/udev` +
`src/libudev`, both on disk when this was written. Point of this doc is not
"port everything systemd has" — most of it exists because systemd owns PID 1,
logind, networkd etc. and udev talks to all of them directly. We don't have
that stack, we have `quantra`. The point is to know exactly what's missing
and which of it we actually need.

`configure.ac` claims `UDEV_VERSION=251` but the code structure (one
`udev-event.c` doing spawn+worker+everything, no varlink, no manager split)
looks older than that. Don't trust the version number.

## Size, roughly

- eudev `src/udev/*.c`: ~12,300 lines
- systemd `src/udev/*.c`: ~20,900 lines

systemd split the old monolithic udevd.c into udev-manager.c /
udev-worker.c / udev-spawn.c / udev-node.c / udev-watch.c / udev-format.c /
udev-dump.c / udev-error.c, and added a varlink control socket
(`udev-varlink.c`) alongside the old netlink/ioctl control interface. We
still have the older single-file shape. That's a structural cleanup, not a
missing feature — not chasing it unless it starts actually hurting us.

## Builtins we don't have

| builtin | systemd file | matters to us? |
|---|---|---|
| `uaccess` | `udev-builtin-uaccess.c` | **yes, see below** |
| `net_setup_link` | `udev-builtin-net_setup_link.c` | yes, see below |
| `tpm2_id` | `udev-builtin-tpm2_id.c` | maybe — `quantra-ramfs` already does its own TPM2 sealing (`tpm2.rs`), check for overlap before adding this |
| `dissect_image` | `udev-builtin-dissect_image.c` | no — GPT auto-discovery is `quantra-ramfs`'s job (`overlay.rs`), not udev's |
| `factory_reset` | `udev-builtin-factory_reset.c` | no, not a thing we do |
| `net_driver` | `udev-builtin-net_driver.c` | low priority, small helper for renaming |

### `uaccess` — actually blocking

`quantra-logind` describes itself as "a `systemd-logind` superset." The
*udev side* of what makes logind's device ACLs work is this builtin plus
`70-uaccess.rules.in` / `71-seat.rules.in` / `73-seat-late.rules.in`: udev
tags devices (`TAG+="uaccess"`), logind grants the ACL to whoever's sitting
at the seat when they log in. We have none of that — no builtin, no rules.

Can't just copy systemd's version, it's built directly against
`sd-login.h`/`login-util.h` (systemd's own session tracking). Would need to
be rewritten against whatever `quantra-logind` exposes instead — probably a
control-socket query, same idea as `quantra-ctl` already talks to `quantra`
over `/run/quantra/control`. This is real work, not a port — flagging it
here so it's a known gap, not doing it in this pass.

### `net_setup_link` — the modern predictable-naming path

We still ship the older `80-net-name-slot.rules`. systemd replaced this
with `.link` file matching + `80-net-setup-link.rules` + `81-net-bridge.rules`
+ `82-net-auto-link-local.rules`. Ours still works, just means anyone
writing `.link` files expecting systemd-networkd conventions won't get
anything. Lower priority than uaccess, but worth knowing.

## Rule files systemd has that we don't

```
60-dmi-id.rules            60-gpiochip.rules         60-infiniband.rules
60-persistent-hidraw.rules 60-persistent-media-controller.rules
60-persistent-storage-mtd.rules (we lump mtd into 75-probe_mtd.rules instead)
60-tpm2-id.rules           65-integration.rules      70-power-switch.rules
70-uaccess.rules  71-seat.rules  73-seat-late.rules  (see above)
80-net-setup-link.rules  81-net-bridge.rules  82-net-auto-link-local.rules
90-image-dissect.rules  90-iocost.rules
90-vconsole.rules.in     ← see below, actually relevant
99-systemd.rules.in       (creates systemd device units, N/A for us)
```

### `90-vconsole.rules.in` — worth pulling in, needs a rewrite

`quantra-ramfs/plymouth.rs` already reads
`/overlayer/syshub/etc/quantra-system/vconsole.conf`, and quantra's own docs
list `vconsole.conf` as a real config file. systemd's rule is what actually
*triggers* vconsole re-application when a VT console device appears:

```
ACTION=="add", SUBSYSTEM=="vtconsole", KERNEL=="vtcon*", \
  RUN+="{{SYSTEMCTL_BINARY_PATH}} --no-block restart systemd-vconsole-setup.service"
```

We have the config file consumer already (`plymouth.rs`) but nothing on the
udev side that fires it on console hotplug. The fix isn't porting the rule
as-is (`systemctl restart` means nothing here) — it's the same rule with
`RUN+=` pointed at whatever `quantra-ctl`/`quantra` exposes for "re-run this
one-shot service." Small, self-contained, and actually plugs a real gap
between two things we already have. Good candidate for the next session
that touches udev.

## `udev.conf` keys we don't support

systemd's `udev.conf` documents `children_max=`, `exec_delay=`,
`event_timeout=`, `timeout_signal=`, `resolve_names=`. We only take the
first two of those from the actual config *file* — `udev-rules.c` /
`libudev.c` only parse `udev_log`. But `udevd.c` already has
`--children-max`, `--exec-delay`, `--event-timeout` as CLI flags and kernel
cmdline options (`udevd.c:1012-1026`), the logic exists — it's just not
wired up to be settable from `udev.conf` the file. Small, mechanical fix if
we ever want it; not urgent since the CLI/cmdline path covers the same
ground.

## What I'm not chasing

- The manager/worker/varlink restructure — real engineering, no functional
  gap, only worth it if we hit a concrete limitation of the current design.
- `dissect_image`, `factory_reset` — belong to `quantra-ramfs`'s job, not
  udev's, on this OS.
- Anything under `src/shared/varlink-io.systemd.Udev.*` — that's systemd's
  IPC bus contract with the rest of systemd, meaningless without the rest
  of systemd.

## If picking one thing to do next

`90-vconsole` wiring, then `uaccess`. vconsole is small and closes a gap
between two pieces of our own stack that already assume it's there.
uaccess is bigger (needs a real design for how udev asks `quantra-logind`
"who's at the seat") but it's the one actual missing feature, not just
staleness.

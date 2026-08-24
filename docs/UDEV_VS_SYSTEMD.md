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

### `90-vconsole.rules` — done

Correction from an earlier version of this doc: `quantra-ramfs/plymouth.rs`
does **not** read `vconsole.conf` — it only reads `rd.vconsole.keymap=` off
`/proc/cmdline` in the initramfs, unrelated to this. The real vconsole
logic is `quantra/src/vconsole.rs::setup()`, called once at boot from
`quantra/src/main.rs:141`.

systemd's rule triggers reapplication on VT hotplug via
`systemctl restart systemd-vconsole-setup.service`. We don't have
`systemctl`, so instead:

- `quantra/src/control.rs` gained a `ReloadVconsole` control-socket verb
  that just calls the same `vconsole::setup()` boot already runs.
- `quantra-ctl reload-vconsole` sends it.
- `rules/90-vconsole.rules.in` fires on `vtcon*` add and runs
  `quantra-ctl reload-vconsole`, reusing the existing `{{ROOTBINDIR}}`
  substitution (same mechanism `64-btrfs.rules.in` already uses).

Implemented, not just flagged.

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

## Status

Both done.

- `90-vconsole` wiring — see above.
- `uaccess` — new builtin (`src/udev/udev-builtin-uaccess.c`), gated
  behind `HAVE_ACL` (configure auto-detects `libacl`, degrades cleanly
  to "not built" if it's missing). Tags devices via
  `rules/70-uaccess.rules` (adapted from systemd's file — the matches
  are generic, nothing systemd-specific there — DRM render/accel/KFD
  nodes tagged unconditionally, `/dev/kvm` deliberately left out, see
  the file for why), triggers the builtin via `rules/90-uaccess-run.rules`.
  Asks `quantra-logind`'s `get_seat`/`get_session` control-socket verbs
  who's active (hand-rolled JSON for those two fixed shapes, no new
  library dependency), grants a POSIX ACL entry on the device node.
  Compiled, linked, and run for real against a live device on this
  machine with `udevadm test-builtin uaccess` — correctly resolves the
  devnode, defaults to `seat0`, and skips cleanly when `quantra-logind`
  isn't reachable (verified — no live `quantra-logind` in this sandbox,
  so that's the actual code path exercised).
  **Not implemented:** ACL revoke on logout/seat-switch — needs
  `quantra-logind` to drive a re-scan, real follow-up work, not done
  here.

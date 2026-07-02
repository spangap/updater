# updater

**updater** is the main-app side of spangap's firmware-update path: a small
trigger that checks a pre-staged update image, sets boot to a dedicated flasher
slot, and reboots. It does not download, flash, or verify anything itself.

A note on the name, because "updater" is overloaded:

- **the `updater` straddle** — this code, which runs as part of the main app and
  *triggers* an update (stage-check + boot-flip).
- **the `updater` app slot** (`ota_1`, partition label `updater`) — a separate,
  permanent app partition that holds the actual flasher. This straddle boots
  *into* it; it does not contain it.
- **the flasher** — the program that runs in that slot, streams the staged image
  to flash, and reboots back into the main app. That program is **not in this
  straddle**.

This straddle supersedes the A/B [`ota`](../ota) straddle.

## The model

One main-app slot (`ota_0`) plus a small, permanent updater slot (`ota_1`). An
update image — a partition table followed by a shrink-wrapped `app` + `fixed` —
is placed on the writable `state` store as `flashme.bin`. The main app sets boot
to the updater slot and reboots; the flasher in that slot writes the image and
reboots back into the new main app. This straddle owns only the main-app side:
post-update cleanup, the stage-check guard, and the boot-flip.

The partitions this relies on ship with the build. When the `updater` straddle is
in the build, the partition generator emits the `otadata` selector and the
`updater` (`ota_1`) slot; the runtime `state` partition is created on first boot.
That layout is a build/core concern documented in
[spangap-core/docs/flash-partitions.md](../spangap-core/docs/flash-partitions.md);
this straddle just looks the partitions up at runtime.

## What this straddle owns

```
updater/
└── esp-idf/
    ├── include/updater.h
    └── src/updater.cpp
```

## Workflow

1. **Stage** the image as `flashme.bin` on the active state store. The path is
   `fsStatePath("/flashme.bin")` — that is `/state/flashme.bin` on flash, or
   `/sdcard/state/flashme.bin` when an SD card is the active store. Staging is
   manual: `scp` over the device's sshd, a WebDAV/file drop, or any write under
   `fsStateDir()`. There is no download/`wget` path **in this straddle**.
2. **Trigger** over the CLI or a storage write:
   - `updater` — guarded: refuses if the staged image would extend over the
     `state` partition.
   - `updater -f` — forced: skips the overwrite guard (and the staged-image
     partition-table read it depends on).
3. The device sets boot to the updater slot and **reboots**. The flasher applies
   the image and reboots back into the new main app. On that next main-app boot,
   `updater` deletes the spent `flashme.bin`.

On any guard failure nothing reboots: the reason is printed to the CLI session
and written to `updater.error`.

`updater` starts automatically when the straddle is in the build — it runs its
boot cleanup, subscribes to the command keys, and registers the CLI verb on its
own. Nothing needs to call an init function.

## CLI

| command      | meaning |
|--------------|---------|
| `updater`    | stage-check + reboot into the updater slot (honors the overwrite guard) |
| `updater -f` | same, but bypass the would-overwrite-`state` guard |

On success the device reboots, so the CLI session simply drops. On failure the
error string is printed. Run on-device with `spangap cli "updater"`.

## Storage interface

These keys are **bare** (no `s.` prefix) and **ephemeral** — they are commands
and status, never persisted.

| key                        | meaning |
|----------------------------|---------|
| `updater.cmd.update`       | write truthy → guarded stage-check + reboot into the updater slot |
| `updater.cmd.update_force` | write truthy → same, bypassing the overwrite guard |
| `updater.error`            | last failure reason; cleared at the start of every attempt |

`updater.error` takes one of these exact values:

| value | when |
|---|---|
| `no image staged` | no `flashme.bin` on the active state store |
| `bad image (no partition table)` | guarded mode: the staged file has no readable partition table at offset `0x8000` |
| `no state partition` | guarded mode: the runtime `state` partition was not found |
| `would overwrite state` | guarded mode: the staged image's declared end extends over the `state` partition |
| `no updater partition` | the `updater` (`ota_1`) app slot was not found |
| `set_boot_partition failed: <name>` | the IDF `esp_ota_set_boot_partition` call failed |

## No signature verification

There is **no signature or crypto verification in this straddle**, by design.
Verification belongs in the (separate, not-in-this-straddle) download/stage path,
before the image is staged; the flasher in the updater slot trusts the staged
file. Keeping verification out means this straddle pulls in no mbedTLS.

## Dependencies

- [spangap-core](../spangap-core) (implicit) — storage, CLI, fs.
- IDF `app_update` / `esp_partition` / `spi_flash` — the boot-flip and partition
  lookup.

## Read next

- [INTERNALS.md](INTERNALS.md) — the partition-table parser, the slot-lookup
  fallbacks, the boot-flip, and maintainer pitfalls.
- [spangap-core/docs/flash-partitions.md](../spangap-core/docs/flash-partitions.md)
  — the floor image, the runtime-grown `state` partition, and the shrink-wrap
  that produce the layout this straddle depends on.

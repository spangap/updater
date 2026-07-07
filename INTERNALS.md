# updater — internals

Maintainer reference for the `updater` straddle. The [README](README.md) is the
operator guide; this document is for changing the code without breaking it. It is
self-authoritative — there is no separate plan document.

The whole straddle is one file, [`esp-idf/src/updater.cpp`](esp-idf/src/updater.cpp),
plus its one-function header. It runs as part of the main app and does three
things: cleans up after a completed update on boot, parses a staged image enough
to guard against clobbering the writable `state` partition, and flips the boot
selector to the flasher slot. It writes no flash itself and verifies nothing.

## 1. Everything this straddle adds

Everything here is new on top of the platform — there is no upstream to fork.

- **Boot cleanup** (`updaterInit`) — the main app is, by definition, running
  *after* a successful update, so any `flashme.bin` left on the state store is
  spent. Delete it on every main-app boot.
- **The command surface** — `storageSubscribeChanges("updater.cmd.")` edge-trigger
  on `updater.cmd.update` / `updater.cmd.update_force`, plus the `updater` CLI
  verb. Both funnel into one `updaterTrigger(force)`.
- **A self-contained partition-table parser** (`stagedImageEnd`) that reads the
  table embedded in the staged image and computes how high on flash it would
  write — used only by the overwrite guard.
- **Partition lookups with label/subtype fallbacks** (`findUpdaterPartition`,
  `findStatePartition`).
- **The boot-flip** — `esp_ota_set_boot_partition(updater_slot)` + `esp_restart()`.

What it deliberately does **not** add: any flashing logic (that lives in the
flasher program in the `updater`/`ota_1` slot, not here), any download/staging
path, and any signature verification or mbedTLS dependency (see §6).

## 2. The partition-table parser (`stagedImageEnd`)

The guard needs to know how far up flash the staged image would write. The image
is `partition-table || app || fixed`, and the table at the front declares where
each partition lands. So the guard parses that table directly out of the file.

The ESP partition table sits at a fixed image/flash offset and is a packed array
of 32-byte little-endian entries:

```c
constexpr uint32_t PART_TABLE_OFFSET = 0x8000;   // table offset within the image
constexpr uint16_t PART_MAGIC        = 0xAA50;    // a real entry's magic
constexpr size_t   PART_ENTRY_SIZE   = 32;
constexpr int      PART_MAX_ENTRIES  = 95;        // defensive cap for the 0x8000..0xC000 region

struct part_entry_t {           // __attribute__((packed)), static_assert == 32 B
    uint16_t magic;
    uint8_t  type;
    uint8_t  subtype;
    uint32_t offset;
    uint32_t size;
    char     label[16];
    uint32_t flags;
};
```

`stagedImageEnd(path, &endOut)`:

1. `fs_open` the staged file, `fs_seek` to `PART_TABLE_OFFSET` (`0x8000`).
2. Read entries one at a time. Stop at the first entry whose `magic != 0xAA50` —
   that terminates the table. (The MD5 checksum sentinel that IDF appends uses a
   different magic, `0xEBEB`, and is not a real entry, so it stops the scan too.)
3. Track `max(offset + size)` over every entry seen, and return it.
4. Return `false` if the table can't be read or has no valid first entry.

**Why the max over *all* entries, not just app+fixed.** The image only carries
`app` + `fixed` (the bootloader and the table itself are skipped; `nvs`/`otadata`
are never in it), but the parser takes the highest end over *every* declared
entry on purpose: anything the table declares is something the flasher could
write, so the conservative bound is "the top of the highest-addressed partition."
The guard compares that bound against `state->address`.

## 3. Slot lookups (label-first, subtype fallback)

Two helpers, both tolerant of how the partition got its identity:

- **`findUpdaterPartition`** — `esp_partition_find_first(APP, ANY, "updater")`
  first (match by label), else `esp_partition_find_first(APP, OTA_1, nullptr)`
  (match by subtype). The shipped table gives the flasher slot the label
  `updater` *and* subtype `ota_1`, so either match resolves it; the fallback
  keeps the trigger working if a board pins the slot with one but not the other.
- **`findStatePartition`** — `esp_partition_find_first(DATA, ANY, "state")`. The
  runtime `state` partition is registered with subtype `SPIFFS` by core's
  `statePartitionEnsure`, so the lookup matches on label with `SUBTYPE_ANY`.

Both can return `null` in principle; the trigger turns that into a clean
`no updater partition` / `no state partition` error rather than dereferencing it.
In the shipped layout the `updater` slot and `state` partition are both present
(the generator emits `otadata` + `updater` when this straddle is staged; fs
creates `state` on first boot), so these are guards against a misconfigured board
table, not the normal path.

## 4. `updaterTrigger(force, err)` — the one path

Shared by the CLI verb and the storage subscription. Returns `false` on any guard
failure (with `err` set and mirrored to `updater.error`); on success it reboots
and never returns.

```
clear updater.error
path = fsStatePath("/flashme.bin")
stat(path)                         -> "no image staged"            (file absent)
if !force:
    stagedImageEnd(path)           -> "bad image (no partition table)"
    findStatePartition()           -> "no state partition"
    imgEnd > state->address        -> "would overwrite state"
findUpdaterPartition()             -> "no updater partition"
esp_ota_set_boot_partition(upd)    -> "set_boot_partition failed: <name>"
esp_restart()                      // does not return
```

**`-f` skips the entire guard block**, including the partition-table read. So a
forced update only requires that the file exists and the updater slot is present;
it does not validate the image at all. That is the escape hatch for an image
whose declared layout trips the guard on purpose — use it knowing nothing checks
the image.

**The overwrite guard is the whole point of the non-forced path.** The runtime
`state` partition lives in the upper half of flash, above `fixed`, and is not in
the shipped table — so a too-large image whose partitions extend past
`state->address` would silently land on top of the live state store when the
flasher writes it. The guard rejects that before the boot-flip. It is a bound on
*declared* layout, not a content check.

## 5. Boot cleanup, command surface, lifecycle

`updaterInit` is invoked from the generated `serviceRunInit()` walk in the
straddle band (after the platform band, so storage / CLI / fs are all up), as an
`init:` hook the generator wraps in an adapter Service. It is plain C++ linkage,
**not** `extern "C"` — the generated dispatch emits a C++ forward declaration. It:

1. Deletes a leftover `fsStatePath("/flashme.bin")` if present (§1). This fires on
   *every* main-app boot, not just post-update — if a `flashme.bin` is sitting
   there for any reason and the main app boots, it gets cleared. Staging then
   triggering is a single atomic intent; a staged image you do not immediately
   trigger does not survive a reboot.
2. Subscribes to the `updater.cmd.` scope and edge-triggers on a truthy
   `updater.cmd.update` (guarded) / `updater.cmd.update_force` (forced). The
   command keys are bare/ephemeral, so an edge-trigger on truthy is the natural
   shape — there is no "clear the command" step because the keys are never
   persisted.
3. Registers the `updater` CLI verb (`-f` detected with `strstr(args, "-f")`).

There is no task, no timer, and no polling — the straddle is dormant between a
boot-time cleanup and a one-shot trigger.

## 6. Why no mbedTLS

Signature/crypto verification is **intentionally absent**. The trust boundary is
the staging step, which is **not in this straddle**: whatever places `flashme.bin`
(a download path, an scp) is responsible for verifying it first. The flasher in
the `updater` slot then trusts the staged file unconditionally, and this trigger
only checks geometry, never authenticity. Keeping verification out is what lets
the straddle stay tiny and free of an mbedTLS dependency. Do not add a signature
check here — it belongs in the stage path, on the producing side.

## 7. Maintainer pitfalls

- **The boot-flip is irreversible from here.** Past `esp_ota_set_boot_partition` +
  `esp_restart` there is no return and no rollback in this code — correctness
  rests entirely on the guard (or the operator's judgment under `-f`) and on the
  flasher. Everything that can fail must fail *before* the restart.
- **`-f` disables the partition-table read, not just the comparison.** Under force
  there is no `bad image` / `no state partition` / `would overwrite state` check
  at all; the only remaining gates are file-exists and updater-slot-exists. Don't
  assume a forced trigger validated the image.
- **The overwrite bound is `state->address`, computed at runtime.** `state` is
  registered by core in the upper half of flash and its address depends on the
  real chip size, so the guard is only meaningful once `statePartitionEnsure` has
  run (it has, by the straddle band). Don't move the trigger earlier in init or
  reimplement the `state` geometry here — read it from the partition API.
- **`stagedImageEnd` reads through the fs layer**, so it works whether the active
  state store is on-flash (`/state`) or SD (`/sdcard/state`). Always build the
  path from `fsStatePath()` / `fsStateDir()`; never hard-code `/state`.
- **The flasher and the partition layout are not this straddle's to change.** The
  `otadata` + `updater` (`ota_1`) slots are emitted by core's partition generator
  when this straddle is staged, and the flasher program lives in that slot. If the
  layout or the flasher protocol changes, that is a core/flash-partitions and
  flasher concern — this trigger only does lookup + boot-flip.

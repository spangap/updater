/**
 * updater — app-side staging + trigger for the single main-app +
 * updater-partition firmware update model.
 *
 * This straddle stages-and-triggers only. It does NOT flash anything itself
 * and does NOT verify signatures — the staged file is trusted:
 *   - On boot it deletes a leftover /state/flashme.bin (post-update cleanup).
 *   - On `updater.cmd.update` / `updater.cmd.update_force` (or the `updater`
 *     CLI verb) it checks the staged /state/flashme.bin, guards against
 *     overwriting the `state` partition (unless forced), sets boot to the
 *     updater app slot, and reboots into it. The updater partition is the
 *     actual flasher.
 *
 * Storage interface (bare/ephemeral keys):
 *   updater.cmd.update        truthy → stage-check + reboot to updater (guarded)
 *   updater.cmd.update_force  same, bypasses the would-overwrite-state guard
 *   updater.error             last failure ("no image staged" /
 *                             "would overwrite state" / ...); cleared each attempt
 */
#ifndef SPANGAP_UPDATER_H
#define SPANGAP_UPDATER_H

#include "service.h"

/** Post-update cleanup, subscribe to updater.cmd.*, register the `updater` CLI
 *  verb. Registered as a boot service; onInit() runs in the straddle band. */
class UpdaterService : public Service {
public:
    void onInit() override;
};

#endif

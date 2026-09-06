# apps/linux: the Linux-only pieces of AC3Forge Crucible

Crucible itself is in [`apps/crucible/`](../crucible/), one application with a platform tree
under it ([docs/crucible/promotion.md](../../docs/crucible/promotion.md), "The platform tree").
What lives here is the part that is not the application: a machine, built to answer one question
about it.

| Directory | What it is |
|---|---|
| [`tray-vm/`](tray-vm/) | A throwaway Debian 13 guest for the Linux tray crash — the one defect Crucible's Linux build currently ships a workaround for. The reference machine is a 2 GB Raspberry Pi that cannot run a sanitiser or hold Qt's debug archive; this is a scripted VMware guest that can. Same shape as [`apps/windows/driver-vm/`](../windows/driver-vm/). |

Nothing here is built or packaged. It is host-side tooling, the way `driver-vm` is.

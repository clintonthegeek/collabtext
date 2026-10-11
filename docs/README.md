# Documentation map

Current consumer entry points:

- [USING_VNEXT.md](USING_VNEXT.md): build/install and public API starting point.
- [consumer-adoption.md](handoff/consumer-adoption.md): ownership, coordinates,
  lifecycle and the explicit master merge gate for Wave/Markoff.
- [wave-core-response.md](handoff/wave-core-response.md): current Wave markers,
  archival readers, local undo and deterministic projection APIs.
- [ALPHA_CAPABILITIES.md](../libs/collabtext/include/collabtext/ALPHA_CAPABILITIES.md):
  installed, versioned capability and serialized-reader authority.

Current maintainer entry points:

- [DIRECTION.md](roadmap/2026-10-reliability/DIRECTION.md): selected lane and policy.
- [TASKS.json](roadmap/2026-10-reliability/TASKS.json): exact dependency/status registry.
- [09-wave-core.md](roadmap/2026-10-reliability/09-wave-core.md): implementable Wave tasks.
- [EXECUTION.md](roadmap/2026-10-reliability/EXECUTION.md): verification procedure.
- [wave-core-adoption.md](specs/wave-core-adoption.md): current normative extension.
- [vnext-operation-codec.md](specs/vnext-operation-codec.md): current frame schemas.
- [vnext-sequence-algorithm.md](specs/vnext-sequence-algorithm.md) §0: native FugueMax.

The remaining vNext specifications describe target contracts and their original
implementation gates; consult registry status and the current extension before
inferring availability. Numbered task procedures remain exact historical steps
when a task is complete. Older dated handoffs/reports, uppercase locator-era
engine/transport/architecture specs, superpowers plans and the alpha.1/alpha.2
release dossier are provenance, not current implementation instructions. Do not
update historical evidence to make it appear to describe a newer candidate.

# Fixed planning inputs

This directory contains the immutable management-centre inputs used by the
`NTN-PLAN-001` seven-day audit. They are kept here so the headless audit can be
replayed from a clean checkout without depending on the ignored Web/GIS
prototype workspace.

- `global-land-l1-v1.json` is the complete 36,411-position L1 catalog. Its
  embedded cells hash is
  `sha256:b39fe9c3ee9a9355b3546036b7f16e0fb858c953f8558cc4295122f2169fbe7a` and
  its file hash is
  `sha256:e2d0b561d427e4c4840fa8088f002dbc639e8a4ecd54d59015596ab3f19f4adf`.
- `onboard-cell-identity-registry.json` contains the two stable NCI/PCI
  identities for each of the 3,528 satellites. Its file hash is
  `sha256:7475821350e104b57a70d979d630f4b29a6cecb89ca0eca7b16dddf2ffee6a4a`.

These files are audit inputs, not Web UI assets and not proof that a selected
constellation provides continuous global service. The scenario manifest binds
their identifiers and hashes before any computation starts.

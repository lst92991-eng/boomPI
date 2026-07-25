# Third-party dependencies

The P1 skeleton does not vendor third-party source code, models, SDK dumps, or
binary libraries. Before adding any dependency, record its exact version,
upstream source, license, SHA-256, footprint, and RV1106 ABI requirements.

Snowboy models/runtime and Rockchip vendor binaries must not be committed until
their redistribution terms have been reviewed. Secrets and downloaded build
artifacts never belong in this directory.

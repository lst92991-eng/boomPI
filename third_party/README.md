# Third-party dependencies

The P1 skeleton does not vendor third-party source code, models, SDK dumps, or
binary libraries. Before adding any dependency, record its exact version,
upstream source, license, SHA-256, footprint, and RV1106 ABI requirements.

The following P0 baselines are installed outside this repository and passed an
initial source, license, checksum, and target-ABI review:

| Dependency | Pinned baseline | License/scope | Repository policy |
| --- | --- | --- | --- |
| [Kitt-AI Snowboy](https://github.com/Kitt-AI/snowboy/commit/c9ff036e2ef3f9c422a3b8c9a01361dbad7a9bd4) | commit `c9ff036e2ef3`; RPi archive SHA-256 `346db1193490a9cc404d49fcfb22ca612cd3a0e649c4863f411553eb1c4f9f1f` | Repository license applies Apache-2.0 to its code, libraries, resources, and default `snowboy.umdl`; other models have separate licenses | External only until packaging review; the public repository's reachable history provides no rebuildable detector core source, and the archive's fatal destructor makes it unsafe in the product process |
| OpenBLAS | commit `1bd74ad3d1e8d21f86d1a6be35abfcdf27c0208a` | BSD 3-Clause | External static library used only behind the Snowboy adapter; any future RV1106 candidate must use an explicit single-thread build |
| [OpenSSL 3.5.7](https://github.com/openssl/openssl/releases/tag/openssl-3.5.7) | source SHA-256 `a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8` | Apache-2.0 | External RV1106 build candidate; do not use the BSP's EOL 1.1.1v as the release baseline |
| Rockchip 3A | Matching BSP `media/common_algorithm/out` artifacts; hashes recorded in the P0 report | Vendor SDK terms; redistribution not yet approved | External SDK input only; reject the glibc/RPATH-contaminated legacy variant |

See `docs/test/p0-feasibility-report-20260725.md` for the ABI evidence and
remaining hardware gates. Snowboy models/runtime and Rockchip vendor binaries
must not be committed until their redistribution terms have been reviewed.
Secrets and downloaded build artifacts never belong in this directory.

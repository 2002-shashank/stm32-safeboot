# Vendored third-party code

| Directory | Upstream | Version | License | Archive SHA-256 |
|---|---|---|---|---|
| `monocypher/` | https://github.com/LoupVaillant/Monocypher | 4.0.2 (`src/monocypher.[ch]`, `src/optional/monocypher-ed25519.[ch]`) | CC0-1.0 or BSD-2-Clause (`LICENCE.md`) | `bc1ca30b1b2654e4e7daf2492c0d204200e55137f23fda6b7142fd7d523bd6b4` |
| `cmsis/core/` | https://github.com/ARM-software/CMSIS_5 | 5.9.0 (`CMSIS/Core/Include`, subset) | Apache-2.0 | `9ce693dd15c41f02506adda79323c077912b3211303ca0d9b28b8ebe735a7031` |
| `cmsis/device/` | https://github.com/STMicroelectronics/cmsis-device-f4 | v2.6.10 (`Include`, subset) | Apache-2.0 (`LICENSE.md`) | `b0566d2e07f6f38d69f3ba78ffa8bbac6f393c5b0fa31dd9e84f2ab42994fe7b` |

Files are unmodified copies. Hashes are of the GitHub release tarballs.
Only `crypto_ed25519_check` (and the SHA-512 it uses) is linked into the
bootloader; `--gc-sections` drops the rest.

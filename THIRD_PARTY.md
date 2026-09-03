# Third-party components

This repository contains project source code under the MIT License, but it interoperates with third-party software that has separate licenses and terms.

## NVIDIA DLSS / NGX

The one-click build clones the official NVIDIA DLSS repository into `external/DLSS`. NVIDIA files are not relicensed by this project. Review NVIDIA's license in that checkout before redistributing NVIDIA binaries.

## FFmpeg

The one-click build reuses an installed FFmpeg or downloads a Windows FFmpeg build for local use. FFmpeg and distributed builds are governed by their own licenses/configuration.

## DLSS-NR engine (src/NeuralEngine)

`src/NeuralEngine.{h,cpp}` is this project's own implementation of the host that
drives DLSS Neural Rendering (NGX feature 18) directly through the driver — no
ReShade or RenoDX add-on. It is original code under this project's MIT license.

What it uses from NVIDIA are interface facts, not third-party code: the NGX
parameter-name strings (e.g. `DLSSNR.Color`), the D3D12 NGX entry-point names,
and the feature id. These are the runtime's own ABI (not a public NVIDIA SDK
API; identified by the community) and are not independently copyrightable. The
module-origin check is satisfied with the standard import-table-redirect method,
implemented here from scratch. An earlier revision vendored files from
Kim2091/Vapourkit (GPL-3.0) for this; they have been fully removed and replaced
by this independent implementation.

## DLSS Super Resolution runtime (nvngx_dlss.dll)

`nvngx_dlss.dll` is the DLSS Super Resolution runtime from the public NVIDIA DLSS
SDK (`external/DLSS/lib/Windows_x86_64/rel`). It is embedded in the player and
extracted beside it, and is redistributed under the terms of that SDK's license
(`external/DLSS/LICENSE.txt`). It is only loaded when the SR upscale is on.

## DLSS-NR runtime (nvngx_dlssnr.dll)

`nvngx_dlssnr.dll` (the neural-rendering runtime the engine loads at run time) is
not included in this repository and is not part of NVIDIA's public SDK. Users
obtain and use it separately under the terms applicable to that file. See
`nr-runtime/README.md`.

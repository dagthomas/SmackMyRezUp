# NR runtime

Place **`nvngx_dlssnr.dll`** (the DLSS Neural Rendering runtime, feature 18) in
this folder.

```
nr-runtime/
    nvngx_dlssnr.dll   <-- put it here
    README.md
```

`build.ps1` looks here first and embeds the DLL into the finished
`SmackMyRezUp.exe` (which self-extracts it on first launch).

## Why it isn't already here

This DLL is **not** part of NVIDIA's public DLSS SDK, so it cannot be
auto-downloaded during the build. It has to be supplied by hand — either dropped
in this folder, or passed as `.\build.ps1 -DlssnrDll C:\path\to\nvngx_dlssnr.dll`.

Use a build that matches your GPU: the stock DLL targets RTX 50-series
(Blackwell) only; a `20_30_40_50` build also runs on RTX 20/30/40-series.

## Committing it to your fork

The DLL is large (~165 MB). If you commit it so clones include it, prefer
[Git LFS](https://git-lfs.com):

```bat
git lfs install
git lfs track "nr-runtime/*.dll"
git add .gitattributes nr-runtime/nvngx_dlssnr.dll
```

Otherwise keep it out of git (see `.gitignore`) and hand it over separately.

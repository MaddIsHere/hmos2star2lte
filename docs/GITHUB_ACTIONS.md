# Building the GPU driver (Mesa llvmpipe) on GitHub Actions

`render_service` draws with **Mesa software GL**. The fast variant, **llvmpipe**, needs LLVM, which is too big to
cross-build on most PCs (about 10 GB of disk, 16 GB of RAM, 1-2 hours). GitHub's free runners do it for you:
`.github/workflows/build-llvmpipe.yml` cross-builds LLVM 19 (AArch64, static) and Mesa for `aarch64-linux-ohos`
and gives you the six libraries as a download.

You only repeat this when you change something under `src/mesa/` or `build/build_mesa.sh` / `build/build_llvm.sh`.
For everything else the libraries already in `patches/11-gpu-mesa/files/` are used.

## What it needs

| Item | Why |
|---|---|
| a **public** GitHub repository with this project | public repositories get the free 4-core / 16 GB runners and plenty of minutes |
| `build/sysroot-base.tar.xz` (in the repo) | open-source headers and crt files |
| `build/sysroot-stubs.tar.xz` (you make it once, step 1) | symbol-only stub libraries to link against. **No Huawei code**: the OTA never leaves your PC |
| secrets `GITEE_USER` and `GITEE_TOKEN` | the OpenHarmony sources are cloned from gitee.com |

## 1. Make the stub sysroot (once, on your PC)

It needs the sysroot that `build/make_sysroot.sh` makes from the OTA images (see [BUILDING.md](BUILDING.md)):

```
bash build/make_sysroot.sh <hmos2star2lte work folder with ota/system.img and ota/vendor.img>
bash build/make_stub_sysroot.sh          # -> build/sysroot-stubs.tar.xz (a few MB)
```

## 2. Put the project on GitHub

1. github.com -> New repository, **Public**, no README / .gitignore / license.
2. In the project folder (WSL):

```
git init -b main                      # skip if it is already a git repo
git remote add origin https://github.com/<you>/hmos2star2lte.git
git add -A
git status --short | head -50         # CHECK what is staged (see below)
git commit -m "Add llvmpipe build via GitHub Actions"
git pull --rebase --autostash origin main     # only if the remote already has commits (README, license...)
git push -u origin main
```

`git push` asks for a **personal access token**, not your password: GitHub -> Settings -> Developer settings ->
Personal access tokens -> Tokens (classic), scopes `repo` and `workflow` (the latter lets you push workflow files).
Or install the GitHub CLI and run `gh auth login`.

What must **not** be committed: the OTA or any `.img` / `.zip` / `.7z`, `work/`, `out*/`, `kernel-out/` images,
and Windows' `*:Zone.Identifier` marker files (`echo '*:Zone.Identifier' >> .gitignore`). A single file over 100 MB
is rejected by GitHub. The `.so` files you copy into `patches/11-gpu-mesa/files/` do not need to be committed either
(`git restore --staged .` if you staged them).

Check that these are in the repository (GitHub's file view, or `git ls-files`): `.github/workflows/build-llvmpipe.yml`,
`build/sysroot-base.tar.xz`, `build/sysroot-stubs.tar.xz`, `build/build_llvm.sh`, `build/build_mesa.sh`,
`build/fetch_oh_sources.sh`, `build/ohos-clang`, `build/ohos-clang++`, `src/mesa/*`.
A file that exists on your disk but is not committed is missing on the runner
(`tar: build/sysroot-base.tar.xz: Cannot open` means exactly that; fix with `git add -f <file>`).

## 3. Add the secrets

Repository -> Settings -> Secrets and variables -> Actions -> New repository secret:

- `GITEE_USER`: your gitee.com user name
- `GITEE_TOKEN`: your gitee password or a private access token

Secrets, not run inputs: on a public repository the inputs of a run are visible to everyone.

## 4. Run it

Actions tab -> "Build Mesa llvmpipe for HarmonyOS (star2lte)" -> **Run workflow**. Inputs (defaults are right):

| Input | Default |
|---|---|
| `mesa_repo` | `https://github.com/lfdevs/mesa-for-android-container.git` |
| `mesa_ref` | `mesa-26.1.0-devel-20260404` (the tree the patches were made on) |
| `llvm_version` | `19.1.7` |

- Always use **Run workflow**. **Re-run jobs** reuses the *old* commit and ignores what you pushed since.
- First run: about **1-2 hours** (LLVM). The LLVM result is cached right after it is built, so later runs only build
  Mesa (about 20-30 minutes). The cache key contains the LLVM version and `build/build_llvm.sh`,
  `build/ohos-clang` and `build/sysroot-stubs.tar.xz`: editing those rebuilds LLVM, editing `build_mesa.sh`,
  `src/mesa/` or the workflow does not. GitHub deletes caches unused for 7 days.
- Only one run at a time (two runs can cancel each other).
- If the log page looks frozen, wait: the web page updates slowly while a step prints thousands of lines.

## 5. Use the result

1. Open the finished run -> **Artifacts** -> `mesa-llvmpipe` (a zip).
2. Unzip it into `patches/11-gpu-mesa/files/lib64/chipsetsdk/`, replacing the six files:
   `libEGL_impl.so`, `libGLESv1_impl.so`, `libGLESv2_impl.so`, `libGLESv3_impl.so`,
   `libgallium-26.1.0-devel.so`, `libexpat.so.1`. `libgallium` is large (LLVM is linked in): make sure SYSTEM has room.
3. Rebuild the ROM (`scripts/build_rom.sh ...`) and flash (`scripts/flash.sh update`). No data wipe is needed.

How to tell it is llvmpipe: the hilog has no explicit line, but in the boot snapshots (`snapN-threads.txt` /
`snapN-stacks.txt`) `render_service` shows threads named `llvmpipe-0` ... `llvmpipe-7`, and `libgallium` is tens of MB.
The `ohos: yuv->rgba ... process memory` lines in the hilog show the video-layer conversion time and `render_service`
memory (it must stay flat).

## When a run fails

| Message | Cause / fix |
|---|---|
| `build/sysroot-stubs.tar.xz is missing` | step 1, then commit it |
| `tar: build/sysroot-base.tar.xz: Cannot open` | the file is not in the repository: `git add -f build/sysroot-base.tar.xz` |
| `GnuTLS recv error` / `early EOF` while cloning from gitee | gitee drops connections from GitHub; `fetch_oh_sources.sh` retries 6 times. Run again |
| `Could not connect to azure.archive.ubuntu.com` | GitHub's package mirror was down: run again |
| `Hunk #2 FAILED ... egllog.c` | harmless (only EGL's own log lines go to hilog); `build_mesa.sh` tolerates exactly that file |
| `Value "ohos" ... is not one of the choices` | `build_mesa.sh` adds `ohos` to Mesa's `platforms` and `egl-native-platform` options; use the current script |
| `undefined symbol: LLVMInitializeAArch64*` | the AArch64 LLVM libraries are passed explicitly in `build_mesa.sh` (the `native` component is empty on an x86 runner) |
| `llvm-strip ... not recognized as a valid object file` | a meson `.symbols` helper file was picked; `build_mesa.sh` now skips them |
| step stops without an error | the runner was cancelled or lost; check the Annotations box of the run and run again |

## Building LLVM yourself (not recommended)

```
export LLVM_PREFIX=$WORK/llvm-ohos
bash build/build_llvm.sh          # needs cmake, ninja, clang/lld, ~10 GB disk, 16 GB RAM, 1-2 h
bash build/build_mesa.sh          # uses llvmpipe when $LLVM_PREFIX/bin/llvm-config-host exists, softpipe otherwise
```

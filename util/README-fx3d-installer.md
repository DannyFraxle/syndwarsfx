# Building the SyndWarsFX 3D update installer

`util/syndwarsfx-fx3d-setup.nsi` builds `syndwarsfx-fx3d-setup.exe`, which updates an
**existing** Syndicate Wars fan port installation with the FX3D OpenGL renderer.

It deliberately does *not* install the game from scratch. The original game content is
copyrighted and must already be present, put there by the standard installer
(`util/syndwarsfx-setup.nsi`) or by `util/install` from your own CD or GOG copy. Because of
that it needs none of the CD/GOG ripping utilities the standard installer requires
(`rip.exe`, `akrip32.dll`, `oggenc.exe`, `Gogisoripper.exe`) — there is no `util-nsis`
dependency at all.

## Prerequisites

1. **NSIS 3.x** — `makensis` on `PATH`.
2. **Three NSIS plugins**, used for the data package download:

   | Plugin | Provides | Source |
   | --- | --- | --- |
   | InetC | `inetc::get` | https://nsis.sourceforge.io/Inetc_plug-in |
   | NSISunzU | `nsisunz::Unzip` | https://nsis.sourceforge.io/Nsisunz_plug-in |
   | Md5dll | `md5dll::GetMD5File` | https://nsis.sourceforge.io/MD5_plugin |

   `make nsis-plugins` fetches and unpacks all three into `nsis-plugins/` under the build
   directory (via `util/getnsisplugins`, which does exactly what the CI workflow does). The
   `.nsi` picks that directory up with `!addplugindir`, so nothing has to be written into
   the NSIS installation directory and no administrator rights are needed. Installing the
   DLLs into `<NSIS>\Plugins\x86-unicode\` by hand works too.

3. A **Windows (MinGW/MSYS2, i686)** build of the project — see the *Building* section of
   the top-level `README.md`.

## Build

From the build directory (`release/` if you followed the README):

```bash
cd release && make nsis-plugins && make pkg-dist && (cd util && makensis syndwarsfx-fx3d-setup.nsi)
```

`make nsis-plugins` only needs running once. The output lands next to the script, as
`release/util/syndwarsfx-fx3d-setup.exe`.

Note that `util/syndwarsfx-fx3d-setup.nsi` in the build directory is a **copy** made by
`configure`, not a symlink - Windows has no symlinks here. After editing the script in the
source tree, run `./config.status` in the build directory to refresh the copy, or you will
quietly rebuild the old one.

`make pkg-dist` runs `util/mkpkgdist`, which does a `make install` into `release/pkg/` and
then resolves the executable's DLL dependencies with `objdump -p`, copying each toolchain
library it finds next to the binary. The DLL list therefore follows the build rather than a
hand-maintained list. The result is `release/pkg/syndwarsfx/`, which the `.nsi` locates
automatically through its `BUILDENV_PKG_DIR` probe.

`makensis` writes `syndwarsfx-fx3d-setup.exe` next to the script.

### Choosing which `fx3d.ini` ships

`make pkg-dist` ships the tracked `conf/fx3d.ini` from the source tree — the
canonical, version-controlled defaults. Its setting values are kept in sync with the tuned
`release/src/conf/fx3d.ini` the game actually reads at runtime; only comments and
blank lines differ between the two, so they are not interchangeable files — keep editing
both in place. If you do want to ship your runtime copy verbatim, pass the directory
explicitly:

```bash
make pkg-dist PKGDIST_CONF=src/conf
```

## What the installer does

- Asks for the existing game folder, prefilled from
  `HKCU\Software\SyndWarsFX\CurrentVersion\InstallPath` (falling back to `HKLM`).
- **Refuses** any folder without `data\` and `qdata\` subfolders — it writes into a
  directory it did not create, so a mistyped path must never reach the install step. It
  never deletes the target folder, on any error path.
- Copies the previous executable and the whole `conf\` folder into
  `<install>\fx3d-backup-<version>\` before replacing anything.
- Installs `syndwarsfx3d.exe`, the runtime DLLs, `conf\` and `language\`.
  `rules.ini` is never overwritten. `fx3d.ini` is installed only if absent; if you
  already have one it is kept and the new defaults are written to `fx3d.ini.new`
  beside it, for you to merge.
- Optionally (deselectable on the components page) downloads the free levels, graphics and
  sound packages from the `swfans/syndwarsfx-{levels,gfx,sfx}` GitHub releases, verifies
  each against the MD5 pinned in `res/syndwarsfx-config.nsh.in`, and extracts them. Deselect
  this section to install offline.
- Writes the install path to `HKCU` **and** `HKLM` (the game checks `HKCU` first; `HKLM` may
  belong to a different account when setup is elevated with other credentials). Skipped
  entirely if *Portable install* is ticked.
- Writes **no uninstaller**. Uninstalling would mean deleting a game installation this
  setup did not create; to revert, restore from the `fx3d-backup-<version>` folder.

## Version and package pinning

Versions and MD5 sums live in `res/syndwarsfx-config.nsh.in`, which `configure` turns into
`res/syndwarsfx-config.nsh`. Update the `LEVELS_`/`GFX_`/`SFX_` triplets there when the
upstream data packages change; `PRODUCT_VERSION` comes from `configure.ac`.

## CI

The GitHub Actions workflow builds only the standard installer. This one is built locally,
on demand.

## The plain zip alternative

Not everyone wants an installer. `util/mkcopyover` repacks the same staged tree into a
"copy this over your installation" archive:

```bash
cd release && make pkg-dist && make pkg-copyover
```

That writes `pkg_dist/SyndWarsFX-fx3d-<version>-<date>.zip`, containing a single top-level
folder with `syndwarsfx3d.exe`, the runtime DLLs, `conf/`, `language/`, the licence files,
a short `INSTALL.txt` and `README-FX3D.md` (a copy of `docs/FX3D-Setup-Guide.md`). The user
unpacks it and copies the contents into their existing game folder.

Unlike the installer it takes no backup and does not keep an existing `fx3d.ini` — the
`INSTALL.txt` and the guide both say so. Override the archive name with
`make pkg-copyover PKGCOPY_NAME=SyndWarsFX-fx3d-nightly`.

`zip` is not part of a stock MSYS2/MinGW install, so the script falls back to Python's
`zipfile` module when `zip` is missing.

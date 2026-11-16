# Building

> [!NOTE]
> This page covers Windows and Linux. The Android build has its own page,
> [ANDROID.md](ANDROID.md), and starts with steps 1 and 2 below.

## 1. Clone the Repository

Clone **ShadowRecomp** with submodules using [Git].

```
git clone --recurse-submodules https://github.com/FoxWorkShop/ShadowRecomp.git
```

### Windows

Run `update_submodules.bat` if you skipped/forgot  `--recurse-submodules`.

## 2. Add the Required Game Files

Copy `default.xbe` from an extracted game and place it inside
`./ShadowRecompLib/private/`.

> [!TIP]
> If you ran recomp before, you can find it in `./game/default.xbe`,
> relative to your executable file.
>
> Otherwise you might need something like `extract-xiso` or `XDVDMulleter` to rip.

## 3. Install Dependencies

### Windows
Grab [Visual Studio 2026].

In installer, select these **Workloads** and **Individual components**:
- Desktop development with C++
- C++ Clang Compiler for Windows
- C++ CMake tools for Windows

### Linux

Or equivalents for your distro...

#### Debian/Ubuntu Based (apt)

If `apt` is your package manager:

```bash
sudo apt install autoconf autoconf-archive automake libtool libltdl-dev \
  python3-venv pkg-config curl cmake ninja-build clang clang-tools \
  libgtk-3-dev
```

#### Arch Based (pacman)

```bash
sudo pacman -S base-devel ninja lld clang gtk3 zip ibus autoconf-archive
```

#### Nix

Use the flake (`flake.nix`) and you good.

## 4. Recompiler

> [!TIP]
> Skip unless you have issues or want to source build recompiler.

[XboxRecomp2] turns `default.xbe` into C++ at build time.

By default, CMake builds it from source if [Rust] is installed, otherwise it
downloads a release binary.

Optional CMake flags:

- `-DSHADOW_RECOMP_XBOXRECOMP2=source|binary`: force one method.
- `-DSHADOW_RECOMP_XBOXRECOMP2_EXECUTABLE=/path/to/xboxrecomp2`: use a binary
  you already have.

## 4. Build the Project

> [!TIP]
> Consider using  `RelWithDebInfo` if you want fast runtime perf with debug info.

### Windows
1. Open the repository folder in Visual Studio.
2. Under **Solution Explorer**, right-click and choose
   **Switch to CMake Targets View**.

![](./assets/windows-setup-1.webp)

3. Right-click **ShadowRecomp**, choose **Set as Startup Item**, then choose
   **Add Debug Configuration**.

![](./assets/windows-setup-2.webp)

4. Click `Edit in launch.vs.json`

![](./assets/windows-setup-3.webp)

And add a `currentDir` and `args` property.

This will prevent you from having to reinstall game when switching between
configurations.

5. Start **ShadowRecomp**. May take a while the first time due to code and
   shader recompilation.

### Linux

1. From the repository root, configure with CMake.
```bash
cmake . --preset linux-release
```

> [!NOTE]
> The available presets are `linux-debug`, `linux-relwithdebinfo` and
> `linux-release`.

2. Build using the selected configuration.
```bash
cmake --build ./out/build/linux-release --target ShadowRecomp
```

3. Run to wherever game was built, run the game.
```bash
./ShadowRecomp
```

### Nix

```bash
# Enter the dev shell (or use direnv: it loads automatically via .envrc)
nix develop

# Build / run (available: debug, relwithdebinfo, release)
build-release
run-release

# Other commands
sr-help
```

All dependencies (SDL3, freetype, GTK, Vulkan loader, ...) come from nixpkgs.
vcpkg is replaced by a stub.

Outside of that, all else works relatively normal.

### VSCode Intellisense

Note: If you're using VSCode as your IDE:
- Install direnv & direnv extension.
- Install clangd extension
- Run `configure-release`.
  - This will generate a `compile_commands.json` file in the root of the
    project.
- Ctrl+Shift+P → `clangd: Restart language server`.

[Git]: https://git-scm.com/
[GitHub CLI]: https://cli.github.com/
[Releases]: https://github.com/FoxWorkshop/ShadowRecomp/releases
[Rust]: https://rustup.rs/
[Visual Studio 2026]: https://visualstudio.microsoft.com/downloads/
[XboxRecomp2]: https://github.com/Sewer56/XboxRecomp2

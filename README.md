# Papers, Please — Nintendo Switch port (Unity 2022.3 / IL2CPP wrapper)
 
This is a native wrapper / loader that runs the original ARM64 Android build of
*Papers, Please* on Switch homebrew.

## Install & run
 
You need files from `Papers Please 1.4.15.128.apk`.
 
Put the `.nro` in **any folder** under `sdmc:/switch/` and place your game files
next to it — the loader finds its folder at runtime, so the name is up to you:

 ```
sdmc:/switch/papers_please
├── papersplease_nx.nro
├── libmain.so  libunity.so  libil2cpp.so   <- from your APK: lib/arm64-v8a/
├── cursor.png                              <- optional
└── assets/                                 <- from your APK: the whole assets/ folder
```
Launch via **title override** (hold **R** while starting an installed game) or a
forwarder.

Optionally drop a **`cursor.png`** (up to 64×64, transparency respected) in the
same folder to replace the on-screen cursor with your own

## Controls
 
Handheld uses the **touchscreen**, exactly like Android — which suits this game,
since it is played by dragging and stamping documents. Everywhere else an
on-screen cursor stands in for a finger.
 
| Input | Action |
| --- | --- |
| **+** | Toggle the on-screen cursor |
| **–** | Toggle gyro pointing (tilt/turn the controller to aim) |
| **Left stick** | Move the cursor |
| **L** / **R** | Recenter the cursor to the middle of the screen (helps gyro aiming) |
| **A** / **ZR** / **ZL** | Tap / confirm (ZL and ZR let you play one-handed) |
| **B** | Android Back |
| **D-pad up / down** | Adjust sensitivity of whatever is driving the cursor |
 
A USB mouse works in both handheld and docked: move to control the cursor,
left-click to tap, and use the scroll wheel to change sensitivity. Your stick,
mouse and gyro sensitivities are remembered in `pointer.cfg` automatically after
in-game adjustment.

## Building
 
Requires devkitPro with the `switch-dev` group plus these portlibs:
 
```sh
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-libpng switch-zlib
```
 
```sh
export DEVKITPRO=/opt/devkitpro
make                        # -> papersplease_nx.nro
```
 
Set `DEBUG_LOG` to `1` in `source/config.h` to get a `debug.log` next to the
`.nro`; it is the first place to look if something fails.

## Credits
 
The loader/shim infrastructure (`so_util`, `libc_shim`, `jni_fake`, `unity_jni`,
`opensles`, `nx_pointer`, diagnostics) derives from the open-source Switch
`.so`-loader lineage — Andy Nguyen, fgsfds and ChanseyIsTheBest, building on
TheOfficialFloW's Vita/Switch loader tradition — reaching this project via the
**Zookeeper DX** port, which shares this game's Unity version and portrait
orientation. All MIT-licensed. Thanks to everyone in that lineage for making this
approach possible.

## Legal
 
No affiliation with Lucas Pope or 3909 LLC. "Papers, Please" is the property of
its owner. **This repository contains no assets or program code from the game,
and none may be distributed with builds.** Wrapper source is MIT (see `LICENSE`).
 

# Quest DOOM

DOOM running on Meta Quest standalone VR headsets (Quest 2 / Pro / 3 / 3S),
built on the [doomgeneric](https://github.com/ozkl/doomgeneric) engine with an
OpenXR + Android NDK backend.

The game renders its software framebuffer onto a large curved, world-locked
panel in front of you — presented in true stereo (each eye gets its own
swapchain image and view/projection) with full 6DOF head tracking. You can lean
in, walk around the panel, and play with the Touch controllers.

## Controls (Oculus Touch)

| Input | Action |
|---|---|
| Left stick | Move forward/back, strafe left/right |
| Right stick | Turn left/right |
| Right trigger | Fire |
| A | Use / open doors |
| B | Escape / menu back |
| Y | Enter / menu confirm |
| X | Automap |
| Left squeeze (hold) | Weapon select mode: X=shotgun(2) A=chaingun(3) B=rocket(4) Y=plasma(5) left-stick up=BFG(6) down=pistol(1) right=chainsaw(7) |

## Building

Requirements:

- Android SDK with platform `android-34` and build-tools
- Android NDK `27.2.12479018` (`sdkmanager "ndk;27.2.12479018" "cmake;3.22.1"`)
- JDK 17
- Gradle 8.9 (or use the included wrapper)

```bash
git clone https://github.com/CommunityPokeOrg/quest-doom.git
cd quest-doom
export ANDROID_HOME=/path/to/Android/Sdk   # or create local.properties with sdk.dir=...
./gradlew assembleDebug
```

Output: `app/build/outputs/apk/debug/app-debug.apk`

The OpenXR loader (`libopenxr_loader.so`, Khronos loader 1.1.38) is vendored in
`app/src/main/jniLibs/` and the OpenXR headers in `app/src/main/cpp/openxr/` —
no external native dependencies are fetched at build time.

## Installing on Quest

Enable developer mode on the headset, then:

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Launch from the app library (filter: unknown sources) or:

```bash
adb shell am start org.communitypoke.questdoom/android.app.NativeActivity
```

## Game data (IWAD)

The shareware `doom1.wad` (freely redistributable) is bundled in
`app/src/main/assets/wads/` and extracted to the app's files directory on first
launch, so the game works out of the box.

To use a retail IWAD instead (doom.wad, doom2.wad, plutonia.wad, tnt.wad,
freedoom1/2.wad, chex.wad, hacx.wad), push it into the app files directory:

```bash
adb push doom2.wad /sdcard/Android/data/org.communitypoke.questdoom/files/
```

The app scans the files directory first, then `/sdcard/`, and picks the first
match in this order: doom2, doom, doom1, plutonia, tnt, freedoom2, freedoom1,
chex, hacx. Savegames and `default.cfg` live in the same files directory.

## Logging

```bash
adb logcat -s QuestDOOM DoomCore
```

## Architecture

```
app/src/main/cpp/
  doomgeneric/    vendored doomgeneric engine (GPL-2.0)
  openxr/         Khronos OpenXR headers (Apache-2.0)
  quest/
    quest_doom.c  android_main, doomgeneric DG_* platform hooks, IWAD discovery,
                  key queue, stdout->logcat
    xr_engine.c   OpenXR instance/session/spaces, EGL+GLES3 context,
                  per-eye swapchains, frame loop
    gl_renderer.c framebuffer -> texture, curved panel mesh, stereo draw
    xr_input.c    OpenXR action system -> DOOM key events (+haptics)
    quest_stubs.c stubs for joystick/ENDOOM subsystems (not used on Quest)
```

The doomgeneric core is unmodified except for removing SDL-dependent files
(`i_system.c` SDL include, `i_joystick.c`, `i_cdmus.c`, `i_endoom.c`) which are
replaced by the stubs and the OpenXR input path.

## Known limitations

- No audio yet (doomgeneric sound modules are compiled out; PRs welcome —
  `FEATURE_SOUND` + an AAudio/OpenSL backend is the obvious next step).
- The 3D world is displayed on a flat panel (like a floating screen), not a
  re-rendered stereoscopic pair — DOOM's software renderer only produces one
  view. A "true stereo" mode would require rendering the scene twice with an
  IPD offset (see `R_RenderPlayerView`).
- Head look does not rotate the player view; turning is on the right stick.

## License

- doomgeneric / DOOM engine code: GPL-2.0 (see `COPYING`)
- Quest platform layer (`app/src/main/cpp/quest/`): GPL-2.0, same as upstream
- OpenXR headers & `libopenxr_loader.so`: Apache-2.0, Khronos Group
- `doom1.wad`: id Software shareware, freely redistributable

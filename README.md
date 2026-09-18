# Quest DOOM

DOOM running on Meta Quest standalone VR headsets (Quest 2 / Pro / 3 / 3S),
built on the [doomgeneric](https://github.com/ozkl/doomgeneric) engine with an
OpenXR + Android NDK backend.

This is a fully immersive port: in-level gameplay is first-person — the DOOM
software renderer runs once per eye (real IPD separation into separate per-eye
swapchains), 6DOF head tracking drives the in-game camera, and the right-hand
controller aim is decoupled from view pitch for weapon aiming. Non-level
content (title, menus, intermissions, demo playback) renders on a
world-locked panel at a fixed pose in the app space — never glued to the HMD.
Meta's `XR_EXT_hand_tracking` adds full gesture controls with a tracked
skeleton (26 joints + bone segments per hand). The shareware `doom1.wad` is
bundled, so it plays out of the box.

## Controls (Oculus Touch)

| Input | Action |
|---|---|
| Head yaw | Turn (your body follows your gaze) |
| Head pitch | Look up/down (Y-shear, see limitations) |
| Head position | Lean / peek around corners (clamped ±0.75 m) |
| Right controller aim pitch | Weapon sprite pitch, decoupled from view |
| Left stick | Move forward/back, strafe left/right |
| Right stick | Turn left/right (alternative to head yaw) |
| Right trigger | Fire |
| A | Use / open doors |
| B | Escape / menu back |
| Y | Enter / menu confirm |
| X | Automap |
| Left squeeze (hold) | Weapon select mode: X=shotgun(2) A=chaingun(3) B=rocket(4) Y=plasma(5) left-stick up=BFG(6) down=pistol(1) right=chainsaw(7) |

## Controls (hand tracking)

Hand tracking uses `XR_EXT_hand_tracking` (26 joints per hand, both hands) and
works in parallel with the Touch controllers — whichever you use wins. If the
runtime or permission doesn't allow it, the game silently falls back to
controllers only. When your hands are tracked, a tracked skeleton renders in-world: small
knuckle cubes at all 26 joints plus bone segments connecting them
(wrist → metacarpals → proximals → intermediates → distals → tips, plus palm
webbing). Warm tint = left hand, cool = right. Everything is depth-tested in
the app space with real per-eye view/FOV matrices.

| Gesture | Action |
|---|---|
| Right index pinch | Fire |
| Right middle pinch | Use / open doors |
| Right ring pinch | Enter / menu confirm |
| Right pinky pinch | Escape / menu back |
| Left index pinch | Automap |
| Left middle / ring / pinky pinch | Select weapon 2 / 3 / 4 |
| Left palm-up + move hand | Virtual move stick (forward/back, strafe) |
| Right palm-up + move hand | Virtual turn stick (left/right) |

The palm-up "virtual joystick" anchors where your palm is when it first faces
up; move your hand off that anchor past a ~10 cm ring to hold a direction —
like a thumbstick springing back to centre.

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

For hand tracking, grant the permission (the app requests
`com.oculus.permission.HAND_TRACKING` and declares
`oculus.software.handtracking` as optional):

```bash
adb shell pm grant org.communitypoke.questdoom com.oculus.permission.HAND_TRACKING
```

Hand tracking must also be enabled on the headset
(Settings → Movement tracking → Hand tracking).

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
    vr_doom.c/.h  VR glue inside the renderer: stereo eye selection, per-eye
                  framebuffer buffers, head-pose -> camera offsets, pitch shear
    r_main.c      viewx/y/z += head+eye offsets; centery pitch shear
    r_plane.c     floor/ceiling slope lookup follows the sheared horizon
    r_things.c    weapon psprite: view-lock compensation + aim-pitch offset
    d_main.c      stereo path renders D_Display() twice per tick
  openxr/         Khronos OpenXR headers (Apache-2.0)
  quest/
    quest_doom.c  android_main, doomgeneric DG_* platform hooks, IWAD discovery,
                  key queue, pose extraction (head/eye/aim), stdout->logcat
    xr_engine.c   OpenXR instance/session/spaces (incl. XR_EXT_hand_tracking
                  enumeration), EGL+GLES3 context, per-eye swapchains
    gl_renderer.c per-eye doom frame textures + instanced joint cubes, drawn
                  into the acquired swapchain image with real view/FOV/projection
    xr_input.c    OpenXR action system -> DOOM key events (+haptics), aim-pose
                  action space for the right controller
    xr_hands.c    XR_EXT_hand_tracking: 26 joints/hand, pinch + palm-up
                  virtual-joystick gestures -> key events
    quest_stubs.c stubs for joystick/ENDOOM subsystems (not used on Quest)
```

### How the VR layer works

DOOM's software renderer is column-based with a 2D map camera, so true 6DOF is
approximated:

- **Head yaw** is accumulated into `player->mo->angle` as deltas, so movement
  direction and hitscan origin always match where you're looking. The original
  yaw is captured on the first tracked pose and used as the XR→map baseline.
- **Head pitch** uses classic Y-shear: `centery`/`centeryfrac` shift the
  horizon and `r_plane.c` indexes `yslope` off the sheared rows so floors and
  ceilings follow. Clamped to ±60 px of shear.
- **Head position** (lean/peek) offsets `viewx/viewy/viewz` inside
  `R_SetupFrame`, rotated from XR local space into the map via the captured
  baseline. Clamped to ±0.75 m so you can't step through walls.
- **Stereo**: `doomgeneric_Tick` runs `D_Display()` once per eye into separate
  framebuffers; `vr_eye` selects which per-eye offset `R_SetupFrame` applies.
  Offsets come from the located `XrView` poses minus the head pose, so real
  IPD is preserved including head yaw.
- **Weapon aim**: the weapon psprite is counter-shifted by the view-pitch
  shear (keeping it view-locked) then offset by right-controller aim pitch
  relative to head pitch — point the controller up/down and the gun follows.
  Aim yaw steering is intentionally left to head yaw.

## Known limitations

- No audio yet (doomgeneric sound modules are compiled out; PRs welcome —
  `FEATURE_SOUND` + an AAudio/OpenSL backend is the obvious next step).
- Pitch is Y-shear, not true projection pitch: the world stays vertically
  aligned and the visible range is limited (±60 px ≈ ±18°). Walls are drawn
  straight-vertical, so looking far up/down still shows a flat horizon edge.
- Weapon aiming is pitch-only; aim *yaw* does not turn the gun independently
  of your head — DOOM's hitscan always originates at the player.
- Hand tracking gestures are discrete (pinch = tap, palm stick = binary
  directions), not analog. If tracking drops mid-gesture, held keys are
  released.
- Stereo renders the full scene twice per tick; on the 640×400 framebuffer
  this is cheap, but the software renderer was never built for this so
  oddities may appear on-map.

## License

- doomgeneric / DOOM engine code: GPL-2.0 (see `COPYING`)
- Quest platform layer (`app/src/main/cpp/quest/`) and VR engine glue
  (`vr_doom.*`): GPL-2.0, same as upstream
- OpenXR headers & `libopenxr_loader.so`: Apache-2.0, Khronos Group
- `doom1.wad`: id Software shareware, freely redistributable

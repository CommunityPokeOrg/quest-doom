# Quest DOOM

DOOM running on Meta Quest standalone VR headsets (Quest 2 / Pro / 3 / 3S),
built on the [doomgeneric](https://github.com/ozkl/doomgeneric) engine with an
OpenXR + Android NDK backend.

In-level gameplay renders the map as **real GLES3 3D geometry**: walls,
floors and ceilings are triangle meshes built per-frame from the parsed level
(sectors / linedefs / subsectors / segs), and monsters/items/enemies are
billboarded sprite quads — all through true per-eye perspective view/projection
matrices with full 6DOF head tracking (real pitch and roll, no Y-shear). The
doomgeneric software renderer still runs underneath for gameplay logic,
automaps and menus; it also drives non-level content (title, menus,
intermissions, demos) on a world-locked panel in app space — never glued to
the HMD. The right-hand controller aim stays decoupled from view pitch for
weapon aiming, and `XR_EXT_hand_tracking` adds a tracked skeleton (26 joints +
bone segments per hand) plus gesture controls. The shareware `doom1.wad` is
bundled, so it plays out of the box.

The project builds two flavors from one code base:

- **quest** — OpenXR backend for Meta Quest (full 6DOF, controllers, hands).
- **cardboard** — generic Android phones / Cardboard-style viewers: side-by-
  side stereo on the window surface with 3DOF head tracking from the
  rotation-vector sensor (no OpenXR, no Google VR SDK).

This 3D-geometry renderer is an **incremental prototype** — see Known
limitations for what's still missing (HUD/weapon viewmodel, sky domes,
visplane-style sprite clipping).

## Controls (Oculus Touch)

| Input | Action |
|---|---|
| Head yaw | Turn (your body follows your gaze) |
| Head pitch | Look up/down (true pitch in the 3D world renderer) |
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
./gradlew assembleQuestDebug        # Quest APK
./gradlew assembleCardboardDebug    # generic Android / Cardboard APK
```

Outputs:

- `app/build/outputs/apk/quest/debug/app-quest-debug.apk`
- `app/build/outputs/apk/cardboard/debug/app-cardboard-debug.apk`
  (package `org.communitypoke.questdoom.cardboard`, label "DOOM Cardboard")

The OpenXR loader (`libopenxr_loader.so`, built from the official Khronos
OpenXR-SDK-Source, release 1.1.63) is vendored for arm64-v8a and armeabi-v7a
in `app/src/main/jniLibs/` and the OpenXR headers in `app/src/main/cpp/openxr/` —
no external native dependencies are fetched at build time.

## Cardboard / phone build

The `cardboard` flavor is a normal Android app: install on any phone with a
rotation-vector sensor (gyroscope) and, optionally, slide it into a
Cardboard-style viewer for stereo.

```bash
adb install -r app/build/outputs/apk/cardboard/debug/app-cardboard-debug.apk
```

What it does:

- Side-by-side stereo: each screen half is one eye (±32 mm IPD, ~60° vertical
  FOV symmetric frustums), using the same `gl_world` 3D level renderer as
  Quest.
- 3DOF head tracking via `SensorManager` `TYPE_ROTATION_VECTOR` (falls back to
  `TYPE_GAME_ROTATION_VECTOR`). Hold the phone in landscape; head yaw/pitch
  aim the view. No positional tracking on phones.
- Runtime backend selection is automatic: `android_main` tries the OpenXR
  backend first and falls back to the sensor/EGL window path when no OpenXR
  runtime exists. Logcat shows `QuestDOOM: BACKEND: OPENXR` or
  `QuestDOOM: BACKEND: CARDBOARD`.
- Touch controls: tap right half = fire, tap left half = use/open. Bluetooth
  or USB gamepads work via key events (A/R1/R2 = fire, X/Y = use, d-pad =
  move, B/back = escape, start = enter).

Cardboard limitations vs Quest: no 6DOF positional tracking, no hand
skeletons, no controller aim decoupling, and no lens-distortion correction
(no Cardboard SDK integration — expect visible warp through lenses).

## Installing on Quest

Enable developer mode on the headset, then:

```bash
adb install -r app/build/outputs/apk/quest/debug/app-quest-debug.apk
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
    gl_renderer.c backend-neutral per-eye compositing: world geometry,
                  doom-frame panel, tracked-hand skeleton; draws via
                  GlrEyeParams (view/proj matrices + viewport + optional
                  texture target) — no OpenXR types
    gl_xr.c/.h    OpenXR eye-draw adapter: acquires swapchain image, converts
                  XrView pose/FOV into GlrEyeParams, applies the compositor
                  projection-Y flip
    cb_engine.c/.h generic Android backend: EGL window surface, SensorManager
                  rotation-vector 3DOF tracking, split-screen eye viewports,
                  touch/gamepad input
    gl_world.c    true-3D level renderer: builds GL triangles from doomgeneric
                  map data (wall quads from segs, floor/ceiling fans from
                  subsector rings, billboard sprites from mobj thinkers),
                  decodes WAD textures/flats/patches via PLAYPAL, and anchors
                  the doom world onto the real head pose in app space
    xr_input.c    OpenXR action system -> DOOM key events (+haptics), aim-pose
                  action space for the right controller
    xr_hands.c    XR_EXT_hand_tracking: 26 joints/hand, pinch + palm-up
                  virtual-joystick gestures -> key events
    quest_stubs.c stubs for joystick/ENDOOM subsystems (not used on Quest)
```

### How the VR layer works

**3D world path (in-level):** `gl_world.c` rebuilds the level as GL triangles
every frame from the engine's live level globals — wall quads from segs
(upper/lower/mid splits from front vs back sector heights), floor/ceiling
triangle fans from each subsector's convex seg ring, and cylindrical billboard
quads for every `P_MobjThinker` thinker (with the correct 8-way sprite
rotation). DOOM wall textures, flats and sprite patches are decoded to GL
textures via PLAYPAL and cached; sector `lightlevel` shades the geometry;
masked midtextures and sprites use an alpha-test pass. A doom→app-space model
matrix anchors the world so the in-game camera lands exactly on the tracked
head pose, then the real `xrLocateViews` pose+FOV per eye supplies the view
transform — true 6DOF with real IPD, pitch and roll.

**Software path (gameplay + 2D content):** DOOM's software renderer is
column-based with a 2D map camera, so on the framebuffer path true 6DOF is
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
- **3D prototype gaps**: no HUD/status bar or weapon viewmodel in the 3D view
  yet (they live in the software frame — shown when the automap/menu opens);
  sky ceilings render as void (no sky dome yet); sprites are not clipped by
  walls/floors per-column like visplanes; no mipmaps; texture pegging
  (`ML_DONTPEGTOP`/`BOTTOM`) is approximated; geometry is rebuilt per frame
  rather than cached with dirty-tracking.
- Weapon aiming is pitch-only; aim *yaw* does not turn the gun independently
  of your head — DOOM's hitscan always originates at the player.
- Hand tracking gestures are discrete (pinch = tap, palm stick = binary
  directions), not analog. If tracking drops mid-gesture, held keys are
  released.
- The software renderer still runs every tick alongside the GL world path
  (needed for automap/menus), so there is some duplicated frame cost.
- **Cardboard flavor**: 3DOF only, no lens-distortion correction (Cardboard
  SDK not integrated), touch controls are minimal (tap-to-fire/use only —
  no virtual sticks yet), gyro yaw drifts without magnetometer fusion.

## License

- doomgeneric / DOOM engine code: GPL-2.0 (see `COPYING`)
- Quest platform layer (`app/src/main/cpp/quest/`) and VR engine glue
  (`vr_doom.*`): GPL-2.0, same as upstream
- OpenXR headers & `libopenxr_loader.so`: Apache-2.0, Khronos Group
- `doom1.wad`: id Software shareware, freely redistributable

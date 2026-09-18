//
// VR support layer for doomgeneric (Quest / OpenXR).
//
// The platform side feeds it 6DOF head + aim poses each frame; the renderer
// consults the globals below in R_SetupFrame / R_MapPlane / R_DrawPSprite.
// Angles arriving from XR are converted to doom units (1 unit ~ 1 inch,
// 1 m = 39.3701 units) and doom angle_t space.
//
#ifndef VR_DOOM_H
#define VR_DOOM_H

#include "doomtype.h"
#include "m_fixed.h"

// When non-zero, doomgeneric_Tick renders one full D_Display per eye into
// vr_screenBuffers[eye] and switches DG_ScreenBuffer between passes.
extern int vr_stereo;

// Current eye being rendered (0=left, 1=right). The platform layer reads this
// inside DG_DrawFrame to know which texture to upload.
extern int vr_eye;

// Render-space offsets applied in R_SetupFrame (doom map units, fixed_t).
extern fixed_t vr_viewofs_x;    // head XZ offset + eye offset, world space
extern fixed_t vr_viewofs_y;
extern fixed_t vr_viewofs_z;    // head height offset

// Pitch Y-shear in viewheight pixels: positive = looking up.
extern int vr_pitch_px;

// Extra vertical shift of the weapon psprite (fixed point fraction pixels),
// used for decoupled controller/hand aim pitch vs view pitch.
extern fixed_t vr_weapon_yofs;

// Platform API -----------------------------------------------------------

// Per-frame pose updates (call before doomgeneric_Tick).
// x/y/z: metres in XR local space; yawDeg/pitchDeg: degrees (pitch+ = up).
void VR_SetHeadPose(float x, float y, float z, float yawDeg, float pitchDeg);
// Aim pose (right controller or dominant hand). tracked=false keeps last
// or falls back to head pitch.
void VR_SetAimPose(float yawDeg, float pitchDeg, int tracked);
// World-space stereo offset for the current eye in metres, derived from the
// located eye views. Called once per frame before rendering.
void VR_SetEyeOffsets(float lx, float lz, float rx, float rz);
// Enable stereo rendering and allocate the second eye buffer.
void VR_EnableStereo(void);
// Select which eye the next D_Display pass writes into.
void VR_SelectEye(int eye);
// Rotate player->mo->angle by head+aim yaw deltas. Called from
// doomgeneric_Tick before TryRunTics.
void VR_Tick(void);
// True when a level is being played (not demo/title) — yaw drives mo->angle.
boolean VR_InLevel(void);

#endif // VR_DOOM_H

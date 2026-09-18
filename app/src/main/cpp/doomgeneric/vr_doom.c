//
// VR support layer for doomgeneric (Quest / OpenXR). See vr_doom.h.
//
// Coordinate conventions:
//   XR local space: +X right, +Y up, -Z forward (right handed).
//   DOOM world: 2D map plane, player faces angle `mo->angle` (0 = +X east,
//   counter-clockwise positive). 1 map unit ~ 1 inch (39.3701 units/metre).
//
//   We fix the XR->DOOM mapping at init: XR -Z (headset forward) maps to
//   whatever direction mo->angle had when the first pose arrived. Thereafter
//   head yaw deltas are accumulated into mo->angle, and any XR-space head or
//   eye offset is rotated by (mo->angle - xrYaw + xrYaw0 - ANG90) so that
//   real-room leaning maps consistently into the map.
//
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "doomgeneric.h"
#include "d_player.h"
#include "doomstat.h"
#include "p_local.h"
#include "tables.h"
#include "vr_doom.h"

#define M_TO_DOOM_UNITS   39.3701f
#define MAX_LEAN_M        0.75f       // clamp room-scale lean/peek distance
#define PITCH_VFOV_DEG    60.0f       // effective vertical FOV for pitch shear
#define PITCH_MAX_PX      60          // keep Y-shear inside sane renderer range

int      vr_stereo      = 0;
int      vr_eye         = 0;
fixed_t  vr_viewofs_x   = 0;
fixed_t  vr_viewofs_y   = 0;
fixed_t  vr_viewofs_z   = 0;
int      vr_pitch_px    = 0;
fixed_t  vr_weapon_yofs = 0;

static pixel_t* vr_screenBufferL = NULL;
static pixel_t* vr_screenBufferR = NULL;

// latest XR poses fed by the platform layer
static float  headX, headY, headZ;      // metres, local space
static float  headYawDeg, headPitchDeg;
static float  aimYawDeg, aimPitchDeg;
static int    aimTracked;
static float  eyeOffL[2], eyeOffR[2];   // metres, XZ offsets from head centre

static int    poseReceived = 0;
static float  prevHeadYawDeg = 0, prevAimYawDeg = 0;
static float  xrYaw0Deg = 0;            // XR yaw when first pose arrived
static float  headY0 = 0;               // head height at first pose

// ---------------------------------------------------------------------------

boolean VR_InLevel(void)
{
    return gamestate == GS_LEVEL
        && players[consoleplayer].mo != NULL
        && !demoplayback
        && !demorecording;
}

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static float wrap_deg(float d)
{
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

static void apply_view_offsets(void)
{
    // Rotate XR-space head/eye offsets into DOOM world space.
    // k = playerAngle - xrYaw + xrYaw0 - 90deg  (see header comment).
    double a = 0, phi = 0, phi0 = 0;

    if (players[consoleplayer].mo != NULL)
        a = (double)players[consoleplayer].mo->angle * (360.0 / 4294967296.0);
    phi = headYawDeg;
    phi0 = xrYaw0Deg;

    double k = (a - phi + phi0 - 90.0) * M_PI / 180.0;

    // room-scale head offset (clamped), XR (x, -z) -> doom axes
    double hx = clampd(headX, -MAX_LEAN_M, MAX_LEAN_M);
    double hz = clampd(headZ, -MAX_LEAN_M, MAX_LEAN_M);
    double vx =  hx * cos(k) + hz * sin(k);
    double vy =  hx * sin(k) - hz * cos(k);

    double ox = (vr_eye == 0 ? eyeOffL[0] : eyeOffR[0]);
    double oz = (vr_eye == 0 ? eyeOffL[1] : eyeOffR[1]);
    double ex =  ox * cos(k) + oz * sin(k);
    double ey =  ox * sin(k) - oz * cos(k);

    vr_viewofs_x = (fixed_t)((vx + ex) * M_TO_DOOM_UNITS * FRACUNIT);
    vr_viewofs_y = (fixed_t)((vy + ey) * M_TO_DOOM_UNITS * FRACUNIT);
    // viewz adds to player->viewz which is already a fixed_t world height
    vr_viewofs_z = (fixed_t)(clampd(headY - headY0, -0.75, 0.75)
                             * M_TO_DOOM_UNITS * FRACUNIT);

    // pitch shear, in viewheight pixels
    int h = 200; // pitch measured against the 200-row virtual screen
    double px = headPitchDeg * (h / PITCH_VFOV_DEG);
    vr_pitch_px = (int)clampd(px, -PITCH_MAX_PX, PITCH_MAX_PX);

    // weapon decoupling: gun sprite follows aim pitch relative to view pitch
    double wp = aimTracked ? (aimPitchDeg - headPitchDeg)
                           : 0.0;
    wp = clampd(wp, -45.0, 45.0);
    vr_weapon_yofs = (fixed_t)(wp * (h / PITCH_VFOV_DEG) * FRACUNIT);
}

void VR_SetHeadPose(float x, float y, float z, float yawDeg, float pitchDeg)
{
    headX = x; headY = y; headZ = z;
    headYawDeg = yawDeg; headPitchDeg = pitchDeg;
    if (!poseReceived)
    {
        poseReceived = 1;
        prevHeadYawDeg = yawDeg;
        xrYaw0Deg = yawDeg;
        headY0 = y;
    }
}

void VR_SetAimPose(float yawDeg, float pitchDeg, int tracked)
{
    if (tracked && !aimTracked)
        prevAimYawDeg = yawDeg;
    aimYawDeg = yawDeg; aimPitchDeg = pitchDeg; aimTracked = tracked;
}

void VR_SetEyeOffsets(float lx, float lz, float rx, float rz)
{
    eyeOffL[0] = lx; eyeOffL[1] = lz;
    eyeOffR[0] = rx; eyeOffR[1] = rz;
}

void VR_EnableStereo(void)
{
    vr_screenBufferR =
        malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * sizeof(pixel_t));
    vr_screenBufferL = DG_ScreenBuffer;   // allocated by doomgeneric_Create
    vr_stereo = 1;
}

void VR_SelectEye(int eye)
{
    vr_eye = eye;
    if (vr_stereo)
        DG_ScreenBuffer = (eye == 1 && vr_screenBufferR)
                          ? vr_screenBufferR
                          : vr_screenBufferL;
    apply_view_offsets();
}

void VR_Tick(void)
{
    if (!poseReceived || !VR_InLevel())
    {
        prevHeadYawDeg = headYawDeg;
        prevAimYawDeg  = aimYawDeg;
        return;
    }

    // Head yaw steers the body: accumulate deltas into mo->angle so that
    // movement direction and hitscan origin match what you are looking at.
    float dyaw = wrap_deg(headYawDeg - prevHeadYawDeg);
    prevHeadYawDeg = headYawDeg;
    if (dyaw != 0.0f)
    {
        // doom angles: positive = counter-clockwise; XR yaw positive = left
        angle_t d = (angle_t)((double)dyaw * (4294967296.0 / 360.0));
        players[consoleplayer].mo->angle += d;
    }
    prevAimYawDeg = aimYawDeg;  // aim yaw currently unused for body steering
}

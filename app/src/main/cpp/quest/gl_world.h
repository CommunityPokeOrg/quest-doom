// True-3D DOOM world renderer (incremental prototype).
//
// Builds GLES triangle geometry from doomgeneric's already-parsed level data
// (subsectors -> segs -> linedefs/sidedefs/sectors, mobj thinkers for sprites)
// and draws it through a real per-eye view matrix — walls, floors, ceilings
// and billboarded things are actual 3D geometry, not a framebuffer quad.
//
// The whole level is mapped into XR app-space each frame: the doom->app model
// matrix is built so that the player's camera position maps onto the real
// tracked head position, which keeps hands/walls in one consistent space.
#pragma once

#include <stdbool.h>

typedef struct GlWorld GlWorld;

// Requires a current GL context. Returns NULL-safe bool.
bool glw_init(GlWorld** out);

// Rebuild the level vertex buffers from the current doom level data.
// Call once per frame after doomgeneric_Tick (sector heights/lights are live).
void glw_begin_frame(GlWorld* w);

// True while a level is loaded and geometry was built.
bool glw_available(const GlWorld* w);

// Head-centred camera position (doom units) for sprite facing; call before
// glw_begin_frame each frame.
void glw_set_frame_camera(GlWorld* w, float camXu, float camYu);

// Position/orient the doom world in app space for this eye.
// headX/Y/Z: head pose in app space (metres). headYawDeg: head yaw (deg).
// camX/Y/Zu: doom camera position in map units (mo pos + vr view offsets).
// moAngleDeg: player->mo->angle in degrees.
void glw_set_camera(GlWorld* w,
                    float headX, float headY, float headZ, float headYawDeg,
                    float camXu, float camYu, float camZu, float moAngleDeg);

// Draw the built level. viewProj is the XR eye view*projection (column-major).
void glw_draw(GlWorld* w, const float* viewProj);

void glw_shutdown(GlWorld* w);

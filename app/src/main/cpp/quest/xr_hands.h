// XR_EXT_hand_tracking support for Quest DOOM.
//
// Queries both hand trackers each frame, exposes the 26 joint poses per hand
// for the GL joint-cube visualisation, and translates gestures into DOOM keys:
//
//   RIGHT hand pinches:  index=fire  middle=use  ring=enter  pinky=escape
//   LEFT  hand pinches:  index=automap(tab)  middle='2' ring='3' pinky='4'
//   Palm-up virtual sticks (hold palm facing up, move hand from its anchor):
//     left hand  -> move forward/back + strafe
//     right hand -> turn left/right (+ vertical pitch would need more than
//                   the shear hack; not mapped)
// Controller input continues to work in parallel — gestures and buttons both
// feed the key queue, and whichever source is active wins per key.
#pragma once

#include "xr_engine.h"
#include "xr_input.h"  // XrKeyCallback

#define XR_HAND_JOINTS 26

typedef struct {
    bool extSupported;
    bool active;

    PFN_xrCreateHandTrackerEXT pfnCreateHandTracker;
    PFN_xrDestroyHandTrackerEXT pfnDestroyHandTracker;
    PFN_xrLocateHandJointsEXT pfnLocateHandJoints;

    XrHandTrackerEXT trackers[2];
    bool tracked[2];

    XrHandJointLocationEXT joints[2][XR_HAND_JOINTS];

    // gesture state
    bool pinch[2][4];              // index/middle/ring/little pinch held
    float stickAnchor[2][3];       // palm-up anchor position
    bool stickActive[2];
    bool held[16];
    unsigned char heldCode[16];
} XrHands;

// Detect extension support; call before xrCreateInstance.
bool xrh_extension_supported(XrEngine* e);
// Create trackers + load procs; call after session creation.
bool xrh_init(XrHands* h, XrEngine* e);
// Per-frame update: locate joints, evaluate gestures, push keys via cb.
void xrh_update(XrHands* h, XrEngine* e, XrKeyCallback cb, void* user);
// Fill renderer's joint buffers. Returns number of joints written (0/26/52).
int xrh_get_joints(const XrHands* h, float* outPos, int* visible);
void xrh_shutdown(XrHands* h);

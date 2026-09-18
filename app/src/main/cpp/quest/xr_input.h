// OpenXR controller input → DOOM key queue.
//
// Mapping (Oculus Touch):
//   left stick          move forward/back + strafe left/right
//   right stick         turn left/right
//   right trigger       fire
//   A                   use / open
//   B                   escape / back
//   Y                   enter (menu confirm)
//   X                   automap (tab)
//   left squeeze (held) weapon modifier: X=2 A=3 B=4 Y=5 L-stick up=6
//                       L-stick down=1, L-stick right=7
#pragma once

#include "xr_engine.h"

typedef void (*XrKeyCallback)(int pressed, unsigned char doomKey, void* user);

typedef struct {
    XrActionSet actionSet;

    XrAction actFire;        // float (trigger/value)
    XrAction actSqueeze;     // float (squeeze/value, left hand)
    XrAction actUse;         // bool (a/click)
    XrAction actMenu;        // bool (b/click)
    XrAction actConfirm;     // bool (y/click)
    XrAction actAutomap;     // bool (x/click)
    XrAction actLeftStick;   // vec2
    XrAction actRightStick;  // vec2
    XrAction actSelect;      // bool (simple profile select)
    XrAction actMenuSimple;  // bool (simple profile menu)
    XrAction actAimPose;     // pose  (right controller aim)
    XrAction actHapticL;     // vibration
    XrAction actHapticR;

    XrSpace aimSpace;        // action space for actAimPose, right hand

    XrPath pathHandLeft;
    XrPath pathHandRight;

    // held-key state tracking for press/release transitions
    bool keyHeld[32];
    unsigned char keyHeldCode[32];

    // button edge state
    bool prevUse, prevMenu, prevConfirm, prevAutomap;
    bool prevSelect, prevMenuSimple;
    bool prevFire, prevSqueeze;
} XrInput;

bool xri_init(XrInput* in, XrEngine* e);
void xri_sync(XrInput* in, XrEngine* e, XrKeyCallback cb, void* user);
// Locate the right-hand aim pose in local space at the current frame's
// predicted display time. Returns false when not tracked.
bool xri_get_aim_pose(XrInput* in, XrEngine* e, XrPosef* outPose);
void xri_haptic(XrInput* in, XrEngine* e, int hand, float amplitude,
                XrDuration durationNs);
void xri_shutdown(XrInput* in);

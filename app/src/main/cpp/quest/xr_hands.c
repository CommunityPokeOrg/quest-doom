#include "xr_hands.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../doomgeneric/doomkeys.h"

#define PINCH_ON_M   0.020f   // fingertip distance to register a pinch
#define PINCH_OFF_M  0.032f   // release hysteresis
#define PALM_UP_DOT  0.55f    // cos threshold: palm-back(+Y joint) down = palm up
#define STICK_ON_M   0.02f    // deadzone from anchor before stick engages
#define STICK_KEY_M  0.10f    // metres from anchor to count as a held direction

static const XrHandEXT kHand[2] = {XR_HAND_LEFT_EXT, XR_HAND_RIGHT_EXT};

// key per (hand, finger) pinch: finger order index/middle/ring/little
static const unsigned char kPinchKey[2][4] = {
    {KEY_TAB, '2', '3', '4'},        // left: automap + weapon slots
    {KEY_FIRE, KEY_USE, KEY_ENTER, KEY_ESCAPE},  // right: fire/use/enter/esc
};

static float dist3(const XrVector3f* a, const XrVector3f* b) {
    float dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Rotate v by quaternion q (v + 2*cross(q.xyz, cross(q.xyz,v) + q.w*v)).
static XrVector3f quat_rot(XrQuaternionf q, XrVector3f v) {
    XrVector3f t = {2 * (q.y * v.z - q.z * v.y),
                    2 * (q.z * v.x - q.x * v.z),
                    2 * (q.x * v.y - q.y * v.x)};
    XrVector3f r = {
        v.x + q.w * t.x + (q.y * t.z - q.z * t.y),
        v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
        v.z + q.w * t.z + (q.x * t.y - q.y * t.x),
    };
    return r;
}

static void held_set(XrHands* h, int slot, bool down, unsigned char key,
                     XrKeyCallback cb, void* user) {
    if (h->held[slot] != down) {
        h->held[slot] = down;
        if (down) h->heldCode[slot] = key;
        cb(down ? 1 : 0, h->heldCode[slot], user);
    }
}

bool xrh_extension_supported(XrEngine* e) {
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(NULL, 0, &count, NULL)))
        return false;
    XrExtensionProperties* props =
        malloc(count * sizeof(XrExtensionProperties));
    for (uint32_t i = 0; i < count; i++)
        props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(NULL, count, &count,
                                                       props))) {
        free(props);
        return false;
    }
    bool found = false;
    for (uint32_t i = 0; i < count; i++)
        if (strcmp(props[i].extensionName,
                   XR_EXT_HAND_TRACKING_EXTENSION_NAME) == 0)
            found = true;
    free(props);
    return found;
}

bool xrh_init(XrHands* h, XrEngine* e) {
    memset(h, 0, sizeof(*h));
    h->extSupported = e->handTrackingExt;
    if (!h->extSupported) {
        LOGI("XR_EXT_hand_tracking not supported by runtime");
        return false;
    }

#define LOAD_PFN(name)                                                     \
    if (XR_FAILED(xrGetInstanceProcAddr(                                   \
            e->instance, #name, (PFN_xrVoidFunction*)&h->pfn##name))) {    \
        LOGE("missing proc %s", #name);                                    \
        return false;                                                      \
    }
    LOAD_PFN(CreateHandTracker);
    LOAD_PFN(DestroyHandTracker);
    LOAD_PFN(LocateHandJoints);
#undef LOAD_PFN

    for (int i = 0; i < 2; i++) {
        XrHandTrackerCreateInfoEXT ci = {
            .type = XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT,
            .hand = kHand[i],
            .handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT,
        };
        XrResult r =
            h->pfnCreateHandTracker(e->session, &ci, &h->trackers[i]);
        if (XR_FAILED(r)) {
            LOGW("hand tracker %d creation failed: %s", i,
                 xr_result_string(r));
            return false;  // permission denied or feature off — fall back
        }
    }
    h->active = true;
    LOGI("hand tracking active (26 joints x 2 hands)");
    return true;
}

// joints[xr hand index] -> our [hand][joint]
static bool locate(XrHands* h, XrEngine* e, int hand) {
    XrHandJointsLocateInfoEXT li = {
        .type = XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT,
        .baseSpace = e->appSpace,
        .time = e->frameState.predictedDisplayTime,
    };
    XrHandJointLocationEXT locs[XR_HAND_JOINTS];
    XrHandJointLocationsEXT out = {
        .type = XR_TYPE_HAND_JOINT_LOCATIONS_EXT,
        .jointCount = XR_HAND_JOINTS,
        .jointLocations = locs,
    };
    if (XR_FAILED(h->pfnLocateHandJoints(h->trackers[hand], &li, &out)))
        return false;
    h->tracked[hand] = out.isActive;
    if (out.isActive)
        memcpy(h->joints[hand], locs, sizeof(locs));
    return out.isActive;
}

static bool joint_valid(const XrHands* h, int hand, int j) {
    return h->tracked[hand] &&
           (h->joints[hand][j].locationFlags &
            XR_SPACE_LOCATION_POSITION_VALID_BIT);
}

static void evaluate(XrHands* h, int hand, XrKeyCallback cb, void* user) {
    static const int tips[4] = {
        XR_HAND_JOINT_INDEX_TIP_EXT, XR_HAND_JOINT_MIDDLE_TIP_EXT,
        XR_HAND_JOINT_RING_TIP_EXT, XR_HAND_JOINT_LITTLE_TIP_EXT};

    if (!joint_valid(h, hand, XR_HAND_JOINT_PALM_EXT)) {
        h->stickActive[hand] = false;
        for (int i = 0; i < 4; i++)
            held_set(h, hand * 8 + i, false, 0, cb, user);
        for (int i = 0; i < 4; i++)
            held_set(h, hand * 8 + 4 + i, false, 0, cb, user);
        return;
    }

    const XrHandJointLocationEXT* joints = h->joints[hand];

    // --- finger pinches ---
    if (joint_valid(h, hand, XR_HAND_JOINT_THUMB_TIP_EXT)) {
        for (int f = 0; f < 4; f++) {
            if (!joint_valid(h, hand, tips[f])) continue;
            float d = dist3(&joints[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position,
                            &joints[tips[f]].pose.position);
            bool p = h->pinch[hand][f] ? d < PINCH_OFF_M : d < PINCH_ON_M;
            h->pinch[hand][f] = p;
            held_set(h, hand * 8 + f, p, kPinchKey[hand][f], cb, user);
        }
    }

    // --- palm-up virtual joystick ---
    // Palm joint +Y axis points out of the back of the hand; palm-up means
    // that axis points down (-Y world).
    XrVector3f palmBack =
        quat_rot(joints[XR_HAND_JOINT_PALM_EXT].pose.orientation,
                 (XrVector3f){0, 1, 0});
    bool palmUp = palmBack.y < -PALM_UP_DOT;
    const XrVector3f* pp = &joints[XR_HAND_JOINT_PALM_EXT].pose.position;

    if (!palmUp) {
        h->stickActive[hand] = false;
    } else if (!h->stickActive[hand]) {
        h->stickActive[hand] = true;
        h->stickAnchor[hand][0] = pp->x;
        h->stickAnchor[hand][1] = pp->y;
        h->stickAnchor[hand][2] = pp->z;
    }

    bool fwd = false, back = false, left = false, right = false;
    if (h->stickActive[hand]) {
        float dx = pp->x - h->stickAnchor[hand][0];
        float dz = pp->z - h->stickAnchor[hand][2];
        // beyond the deadzone the anchor is dragged along, giving a
        // relative-stick feel rather than absolute displacement
        if (fabsf(dx) < STICK_ON_M) dx = 0;
        if (fabsf(dz) < STICK_ON_M) dz = 0;
        if (hand == 0) {
            // left hand: fwd/back + strafe
            fwd   = dz < -STICK_KEY_M;
            back  = dz >  STICK_KEY_M;
            left  = dx < -STICK_KEY_M;
            right = dx >  STICK_KEY_M;
        } else {
            // right hand: turn only
            left  = dx < -STICK_KEY_M;
            right = dx >  STICK_KEY_M;
        }
    }
    int base = hand * 8 + 4;
    if (hand == 0) {
        held_set(h, base + 0, fwd, KEY_UPARROW, cb, user);
        held_set(h, base + 1, back, KEY_DOWNARROW, cb, user);
        held_set(h, base + 2, left, KEY_STRAFE_L, cb, user);
        held_set(h, base + 3, right, KEY_STRAFE_R, cb, user);
    } else {
        held_set(h, base + 0, left, KEY_LEFTARROW, cb, user);
        held_set(h, base + 1, right, KEY_RIGHTARROW, cb, user);
        held_set(h, base + 2, false, 0, cb, user);
        held_set(h, base + 3, false, 0, cb, user);
    }
}

void xrh_update(XrHands* h, XrEngine* e, XrKeyCallback cb, void* user) {
    if (!h->active || !e->sessionRunning) return;
    for (int i = 0; i < 2; i++)
        if (locate(h, e, i)) evaluate(h, i, cb, user);
}

int xrh_get_joints(const XrHands* h, float* outPos, int* visible) {
    int n = 0;
    for (int i = 0; i < 2; i++) {
        visible[i] = h->active && h->tracked[i];
        if (visible[i]) {
            for (int j = 0; j < XR_HAND_JOINTS; j++) {
                XrVector3f p = h->joints[i][j].pose.position;
                outPos[n * 3 + 0] = p.x;
                outPos[n * 3 + 1] = p.y;
                outPos[n * 3 + 2] = p.z;
                n++;
            }
        } else {
            // keep buffer positions stable: emit zeros so indices don't shift
            for (int j = 0; j < XR_HAND_JOINTS; j++) {
                outPos[n * 3 + 0] = outPos[n * 3 + 1] = outPos[n * 3 + 2] = 0;
                n++;
            }
        }
    }
    return n;
}

void xrh_shutdown(XrHands* h) {
    if (!h->active) return;
    for (int i = 0; i < 2; i++)
        if (h->trackers[i] != XR_NULL_HANDLE)
            h->pfnDestroyHandTracker(h->trackers[i]);
}

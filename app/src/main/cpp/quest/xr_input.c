#include "xr_input.h"

#include <stdlib.h>
#include <string.h>

#include "../doomgeneric/doomkeys.h"

#define STICK_PRESS_THRESHOLD 0.55f
#define STICK_RELEASE_THRESHOLD 0.40f
#define TRIG_PRESS_THRESHOLD 0.7f
#define TRIG_RELEASE_THRESHOLD 0.5f
#define SLOT_COUNT 32

static XrPath str2path(XrInstance instance, const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(instance, s, &p);
    return p;
}

// subactions: 0=none, 1=left, 2=right, 3=both
static bool make_action(XrInput* in, XrAction* dst, const char* name,
                        const char* lname, XrActionType type, int subactions) {
    XrPath subs[2];
    int n = 0;
    if (subactions & 1) subs[n++] = in->pathHandLeft;
    if (subactions & 2) subs[n++] = in->pathHandRight;
    XrActionCreateInfo ci = {
        .type = XR_TYPE_ACTION_CREATE_INFO,
        .actionType = type,
        .countSubactionPaths = (uint32_t)n,
        .subactionPaths = subs,
    };
    strncpy(ci.actionName, name, sizeof(ci.actionName) - 1);
    strncpy(ci.localizedActionName, lname,
            sizeof(ci.localizedActionName) - 1);
    return XR_SUCCEEDED(xrCreateAction(in->actionSet, &ci, dst));
}

typedef struct {
    XrAction action;
    const char* path;
} Binding;

static bool suggest(XrInput* in, XrInstance instance, const char* profileStr,
                    const Binding* bindings, int count) {
    XrActionSuggestedBinding* list =
        malloc(count * sizeof(XrActionSuggestedBinding));
    for (int i = 0; i < count; i++) {
        list[i].action = bindings[i].action;
        list[i].binding = str2path(instance, bindings[i].path);
    }
    XrInteractionProfileSuggestedBinding sugg = {
        .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
        .interactionProfile = str2path(instance, profileStr),
        .countSuggestedBindings = (uint32_t)count,
        .suggestedBindings = list,
    };
    XrResult r = xrSuggestInteractionProfileBindings(instance, &sugg);
    free(list);
    return XR_SUCCEEDED(r);
}

bool xri_init(XrInput* in, XrEngine* e) {
    memset(in, 0, sizeof(*in));
    in->pathHandLeft = str2path(e->instance, "/user/hand/left");
    in->pathHandRight = str2path(e->instance, "/user/hand/right");

    XrActionSetCreateInfo asci = {
        .type = XR_TYPE_ACTION_SET_CREATE_INFO,
        .priority = 0,
    };
    strncpy(asci.actionSetName, "gameplay", sizeof(asci.actionSetName) - 1);
    strncpy(asci.localizedActionSetName, "Gameplay",
            sizeof(asci.localizedActionSetName) - 1);
    if (XR_FAILED(xrCreateActionSet(e->instance, &asci, &in->actionSet)))
        return false;

    bool ok = true;
    ok &= make_action(in, &in->actFire, "fire", "Fire",
                      XR_ACTION_TYPE_FLOAT_INPUT, 2);
    ok &= make_action(in, &in->actSqueeze, "squeeze", "Weapon Modifier",
                      XR_ACTION_TYPE_FLOAT_INPUT, 1);
    ok &= make_action(in, &in->actUse, "use", "Use / Open",
                      XR_ACTION_TYPE_BOOLEAN_INPUT, 2);
    ok &= make_action(in, &in->actMenu, "menu", "Menu / Back",
                      XR_ACTION_TYPE_BOOLEAN_INPUT, 2);
    ok &= make_action(in, &in->actConfirm, "confirm", "Confirm",
                      XR_ACTION_TYPE_BOOLEAN_INPUT, 2);
    ok &= make_action(in, &in->actAutomap, "automap", "Automap",
                      XR_ACTION_TYPE_BOOLEAN_INPUT, 1);
    ok &= make_action(in, &in->actLeftStick, "leftstick", "Move",
                      XR_ACTION_TYPE_VECTOR2F_INPUT, 1);
    ok &= make_action(in, &in->actRightStick, "rightstick", "Turn",
                      XR_ACTION_TYPE_VECTOR2F_INPUT, 2);
    ok &= make_action(in, &in->actAimPose, "aimpose", "Aim Pose",
                      XR_ACTION_TYPE_POSE_INPUT, 2);
    ok &= make_action(in, &in->actSelect, "select", "Select",
                      XR_ACTION_TYPE_BOOLEAN_INPUT, 3);
    ok &= make_action(in, &in->actMenuSimple, "menusimple", "Menu",
                      XR_ACTION_TYPE_BOOLEAN_INPUT, 3);
    ok &= make_action(in, &in->actHapticL, "hapticl", "Haptics Left",
                      XR_ACTION_TYPE_VIBRATION_OUTPUT, 1);
    ok &= make_action(in, &in->actHapticR, "hapticr", "Haptics Right",
                      XR_ACTION_TYPE_VIBRATION_OUTPUT, 2);
    if (!ok) {
        LOGE("failed to create actions");
        return false;
    }

    // --- Oculus Touch (Quest 1/2/Pro/3 controllers) ---
    const Binding touch[] = {
        {in->actFire, "/user/hand/right/input/trigger/value"},
        {in->actUse, "/user/hand/right/input/a/click"},
        {in->actMenu, "/user/hand/right/input/b/click"},
        {in->actConfirm, "/user/hand/left/input/y/click"},
        {in->actAutomap, "/user/hand/left/input/x/click"},
        {in->actSqueeze, "/user/hand/left/input/squeeze/value"},
        {in->actLeftStick, "/user/hand/left/input/thumbstick"},
        {in->actRightStick, "/user/hand/right/input/thumbstick"},
        {in->actAimPose, "/user/hand/right/input/aim/pose"},
        {in->actHapticL, "/user/hand/left/output/haptic"},
        {in->actHapticR, "/user/hand/right/output/haptic"},
    };
    if (!suggest(in, e->instance, "/interaction_profiles/oculus/touch_controller",
                 touch, sizeof(touch) / sizeof(touch[0])))
        LOGW("oculus touch binding suggestion failed");

    // --- Simple controller fallback ---
    const Binding simple[] = {
        {in->actSelect, "/user/hand/right/input/select/click"},
        {in->actSelect, "/user/hand/left/input/select/click"},
        {in->actMenuSimple, "/user/hand/right/input/menu/click"},
        {in->actMenuSimple, "/user/hand/left/input/menu/click"},
        {in->actHapticR, "/user/hand/right/output/haptic"},
        {in->actHapticL, "/user/hand/left/output/haptic"},
    };
    suggest(in, e->instance, "/interaction_profiles/khr/simple_controller",
            simple, sizeof(simple) / sizeof(simple[0]));

    XrSessionActionSetsAttachInfo attach = {
        .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
        .countActionSets = 1,
        .actionSets = &in->actionSet,
    };
    if (XR_FAILED(xrAttachSessionActionSets(e->session, &attach)))
        return false;

    XrActionSpaceCreateInfo asi = {
        .type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
        .action = in->actAimPose,
        .subactionPath = in->pathHandRight,
        .poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}},
    };
    return XR_SUCCEEDED(xrCreateActionSpace(e->session, &asi, &in->aimSpace));
}

bool xri_get_aim_pose(XrInput* in, XrEngine* e, XrPosef* outPose) {
    if (in->aimSpace == XR_NULL_HANDLE || !e->sessionRunning) return false;
    XrActionStateGetInfo gi = {
        .type = XR_TYPE_ACTION_STATE_GET_INFO,
        .action = in->actAimPose,
        .subactionPath = in->pathHandRight,
    };
    XrActionStatePose st = {.type = XR_TYPE_ACTION_STATE_POSE};
    xrGetActionStatePose(e->session, &gi, &st);
    if (!st.isActive) return false;
    XrSpaceLocation loc = {.type = XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(in->aimSpace, e->localSpace,
                                e->frameState.predictedDisplayTime, &loc)))
        return false;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        return false;
    *outPose = loc.pose;
    return true;
}

// --- state → key queue -------------------------------------------------------

static void slot_set(XrInput* in, int slot, bool down, unsigned char key,
                     XrKeyCallback cb, void* user) {
    if (in->keyHeld[slot] != down) {
        in->keyHeld[slot] = down;
        if (down) in->keyHeldCode[slot] = key;
        cb(down ? 1 : 0, in->keyHeldCode[slot], user);
    }
}

static bool get_bool(XrInput* in, XrEngine* e, XrAction action, XrPath sub) {
    XrActionStateGetInfo gi = {
        .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = action,
        .subactionPath = sub};
    XrActionStateBoolean st = {.type = XR_TYPE_ACTION_STATE_BOOLEAN};
    xrGetActionStateBoolean(e->session, &gi, &st);
    return st.isActive && st.currentState;
}

static float get_float(XrInput* in, XrEngine* e, XrAction action, XrPath sub) {
    XrActionStateGetInfo gi = {
        .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = action,
        .subactionPath = sub};
    XrActionStateFloat st = {.type = XR_TYPE_ACTION_STATE_FLOAT};
    xrGetActionStateFloat(e->session, &gi, &st);
    return st.isActive ? st.currentState : 0.0f;
}

static XrVector2f get_vec2(XrInput* in, XrEngine* e, XrAction action,
                         XrPath sub) {
    XrActionStateGetInfo gi = {
        .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = action,
        .subactionPath = sub};
    XrActionStateVector2f st = {.type = XR_TYPE_ACTION_STATE_VECTOR2F};
    xrGetActionStateVector2f(e->session, &gi, &st);
    return st.isActive ? st.currentState : (XrVector2f){0, 0};
}

void xri_sync(XrInput* in, XrEngine* e, XrKeyCallback cb, void* user) {
    if (!e->sessionRunning) return;

    XrActiveActionSet active = {
        .actionSet = in->actionSet,
        .subactionPath = XR_NULL_PATH,
    };
    XrActionsSyncInfo syncInfo = {
        .type = XR_TYPE_ACTIONS_SYNC_INFO,
        .countActiveActionSets = 1,
        .activeActionSets = &active,
    };
    if (XR_FAILED(xrSyncActions(e->session, &syncInfo))) return;

    // --- buttons (edge transitions) ---
    bool use = get_bool(in, e, in->actUse, in->pathHandRight) ||
               get_bool(in, e, in->actUse, in->pathHandLeft);
    bool menu = get_bool(in, e, in->actMenu, in->pathHandRight) ||
                get_bool(in, e, in->actMenu, in->pathHandLeft) ||
                get_bool(in, e, in->actMenuSimple, in->pathHandRight) ||
                get_bool(in, e, in->actMenuSimple, in->pathHandLeft);
    bool confirm = get_bool(in, e, in->actConfirm, in->pathHandLeft) ||
                   get_bool(in, e, in->actConfirm, in->pathHandRight);
    bool automap = get_bool(in, e, in->actAutomap, in->pathHandLeft);
    bool select = get_bool(in, e, in->actSelect, in->pathHandRight) ||
                  get_bool(in, e, in->actSelect, in->pathHandLeft);

    float fireV = get_float(in, e, in->actFire, in->pathHandRight);
    bool fire = in->prevFire ? fireV > TRIG_RELEASE_THRESHOLD
                             : fireV > TRIG_PRESS_THRESHOLD;
    fire = fire || select;
    float sqV = get_float(in, e, in->actSqueeze, in->pathHandLeft);
    bool squeeze = in->prevSqueeze ? sqV > 0.4f : sqV > 0.6f;

    XrVector2f ls = get_vec2(in, e, in->actLeftStick, in->pathHandLeft);
    XrVector2f rs = get_vec2(in, e, in->actRightStick, in->pathHandRight);

    if (squeeze) {
        // Weapon-select mode: face buttons & left stick emit digit keys.
        slot_set(in, 0, get_bool(in, e, in->actAutomap, in->pathHandLeft), '2',
                 cb, user);   // X -> shotgun
        slot_set(in, 1, use, '3', cb, user);                       // A -> chaingun
        slot_set(in, 2, menu, '4', cb, user);                      // B -> rocket
        slot_set(in, 3, confirm, '5', cb, user);                   // Y -> plasma
        slot_set(in, 4, ls.y > STICK_PRESS_THRESHOLD, '6', cb, user);   // BFG
        slot_set(in, 5, ls.y < -STICK_PRESS_THRESHOLD, '1', cb, user);  // fist/pistol
        slot_set(in, 6, ls.x > STICK_PRESS_THRESHOLD, '7', cb, user);   // chainsaw
        // movement keys released while in weapon mode
        slot_set(in, 8, false, KEY_UPARROW, cb, user);
        slot_set(in, 9, false, KEY_DOWNARROW, cb, user);
        slot_set(in, 10, false, KEY_STRAFE_L, cb, user);
        slot_set(in, 11, false, KEY_STRAFE_R, cb, user);
        slot_set(in, 12, false, KEY_USE, cb, user);
        slot_set(in, 13, false, KEY_ESCAPE, cb, user);
        slot_set(in, 14, false, KEY_ENTER, cb, user);
        slot_set(in, 15, false, KEY_TAB, cb, user);
    } else {
        for (int i = 0; i < 7; i++) slot_set(in, i, false, 0, cb, user);
        slot_set(in, 8, ls.y > STICK_PRESS_THRESHOLD, KEY_UPARROW, cb, user);
        slot_set(in, 9, ls.y < -STICK_PRESS_THRESHOLD, KEY_DOWNARROW, cb, user);
        slot_set(in, 10, ls.x < -STICK_PRESS_THRESHOLD, KEY_STRAFE_L, cb, user);
        slot_set(in, 11, ls.x > STICK_PRESS_THRESHOLD, KEY_STRAFE_R, cb, user);
        slot_set(in, 12, use, KEY_USE, cb, user);
        slot_set(in, 13, menu, KEY_ESCAPE, cb, user);
        slot_set(in, 14, confirm, KEY_ENTER, cb, user);
        slot_set(in, 15, automap, KEY_TAB, cb, user);
    }

    // turning always on right stick
    slot_set(in, 16, rs.x < -STICK_PRESS_THRESHOLD, KEY_LEFTARROW, cb, user);
    slot_set(in, 17, rs.x > STICK_PRESS_THRESHOLD, KEY_RIGHTARROW, cb, user);

    // fire
    slot_set(in, 18, fire, KEY_FIRE, cb, user);

    in->prevUse = use;
    in->prevMenu = menu;
    in->prevConfirm = confirm;
    in->prevAutomap = automap;
    in->prevSelect = select;
    in->prevFire = fire;
    in->prevSqueeze = squeeze;
}

void xri_haptic(XrInput* in, XrEngine* e, int hand, float amplitude,
                XrDuration durationNs) {
    XrHapticActionInfo hi = {
        .type = XR_TYPE_HAPTIC_ACTION_INFO,
        .action = hand == 0 ? in->actHapticL : in->actHapticR,
        .subactionPath = hand == 0 ? in->pathHandLeft : in->pathHandRight,
    };
    XrHapticVibration vib = {
        .type = XR_TYPE_HAPTIC_VIBRATION,
        .amplitude = amplitude,
        .duration = durationNs,
    };
    xrApplyHapticFeedback(e->session, &hi, (XrHapticBaseHeader*)&vib);
}

void xri_shutdown(XrInput* in) {
    // actions die with the action set; destroy the set
    if (in->actionSet != XR_NULL_HANDLE) xrDestroyActionSet(in->actionSet);
}

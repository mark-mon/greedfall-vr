#pragma once
// In-process OpenXR for D3D11 (see xr_inproc.cpp). One call per game Present.
#include <d3d11.h>
#include "xrbridge/client.h"
namespace xrb {
struct Submit11 {
    bool stereo = false;                    // eye images valid this frame
    ID3D11Texture2D* eye[2] = {};
    IpcPose pose[2] = {};                   // the poses / FOVs the eye images were rendered with
    IpcFov fov[2] = {};
    ID3D11Texture2D* panel = nullptr;       // HUD (stereo, premultiplied alpha) or the flat game image (no stereo)
    bool panelIsHud = false;
};
void XrFrame11(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Submit11& s);   // copies + xrEndFrame (begins the frame itself if needed)
void XrBeginFrame11(bool enabled);
void XrSetEyeTracking(bool on);                            // read the eye tracker each frame (foveated rendering)
bool XrGetGaze(float dir[3]);                              // latest gaze direction in the pose space (false = none)
int XrEyeTrackingStatus();                                 // -1 runtime lacks it, -2 headset lacks it, 0 starting, 1 ready (no gaze), 2 tracking
void XrSetSimulatedGaze(float amplitudeDeg, float periodSec);   // test aid: the gaze circles around the view centre
void XrSetFixLateTime(bool on);    // replace display times that are already in the past
void XrSetSimulatedHead(float pitchDeg, float yawDeg, float periodSec, float fixedPitchDeg, float fixedYawDeg = 0);   // test aid: fake head motion in the OpenXR poses   // right after the game's Present: xrWaitFrame + xrBeginFrame + pose for the next frame
}

// The slot tables kl_openvr serves, one X-list per interface version:
// X(TAG, slot, Method, KIND). Transcribed from hla/OPENVR-VTABLES.md §3, whose
// method names were checked row for row against the public headers
// (ValveSoftware/openvr v2.15.6 for System_026, Compositor_029, Input_011,
// Overlay_028, Chaperone_004, OverlayView_003, RenderModels_006,
// HeadsetView_001; v2.5.1 for Applications_007) when this file was written.
//
// KIND is IMPL (the runtime implements it: klvr_<TAG>_<Method>) or a stub kind
// naming the register its zero lands in — see kl_openvr_int.h. A stub's zero is
// the answer the reference host's stub gave (0, or 13 for IVRInput), except
// that a const char * stub answers "" rather than NULL.
//
// The three private interfaces have no header. Mailbox_001's four slots are
// INFERRED (§3.10); XrProto_001 and InputInternal_002 are UNKNOWN by name.
// Each is served 32 slots wide — a known prefix and logging stubs past it — so
// a slot the recon never saw is a named log line, not a jump through whatever
// follows the table.
//
// The slot counts are compile-time checked (KLVR_INTERFACE) and the whole
// table is walked by tests/t_openvr.c.
#ifndef KL_OPENVR_SLOTS_H
#define KL_OPENVR_SLOTS_H

#define KLVR_SLOTS_System(X) \
    X(System, 0, GetRecommendedRenderTargetSize, IMPL) \
    X(System, 1, GetProjectionMatrix, IMPL) \
    X(System, 2, GetProjectionRaw, IMPL) \
    X(System, 3, ComputeDistortion, IMPL) \
    X(System, 4, ComputeDistortionSet, Z) \
    X(System, 5, GetEyeToHeadTransform, IMPL) \
    X(System, 6, GetTimeSinceLastVsync, Z) \
    X(System, 7, GetD3D9AdapterIndex, Z) \
    X(System, 8, GetDXGIOutputInfo, Z) \
    X(System, 9, GetOutputDevice, Z) \
    X(System, 10, IsDisplayOnDesktop, Z) \
    X(System, 11, SetDisplayVisibility, Z) \
    X(System, 12, GetDeviceToAbsoluteTrackingPose, IMPL) \
    X(System, 13, GetSeatedZeroPoseToStandingAbsoluteTrackingPose, IMPL) \
    X(System, 14, GetRawZeroPoseToStandingAbsoluteTrackingPose, IMPL) \
    X(System, 15, GetSortedTrackedDeviceIndicesOfClass, Z) \
    X(System, 16, GetTrackedDeviceActivityLevel, Z) \
    X(System, 17, ApplyTransform, Z) \
    X(System, 18, GetTrackedDeviceIndexForControllerRole, IMPL) \
    X(System, 19, GetControllerRoleForTrackedDeviceIndex, IMPL) \
    X(System, 20, GetTrackedDeviceClass, IMPL) \
    X(System, 21, IsTrackedDeviceConnected, IMPL) \
    X(System, 22, GetBoolTrackedDeviceProperty, IMPL) \
    X(System, 23, GetFloatTrackedDeviceProperty, IMPL) \
    X(System, 24, GetInt32TrackedDeviceProperty, IMPL) \
    X(System, 25, GetUint64TrackedDeviceProperty, IMPL) \
    X(System, 26, GetMatrix34TrackedDeviceProperty, IMPL) \
    X(System, 27, GetArrayTrackedDeviceProperty, Z) \
    X(System, 28, GetStringTrackedDeviceProperty, IMPL) \
    X(System, 29, GetPropErrorNameFromEnum, S) \
    X(System, 30, PollNextEvent, IMPL) \
    X(System, 31, PollNextEventWithPose, IMPL) \
    X(System, 32, PollNextEventWithPoseAndOverlays, Z) \
    X(System, 33, GetEventTypeNameFromEnum, S) \
    X(System, 34, GetHiddenAreaMesh, IMPL) \
    X(System, 35, GetEyeTrackedFoveationCenter, Z) \
    X(System, 36, GetEyeTrackedFoveationCenterForProjection, Z) \
    X(System, 37, GetControllerState, Z) \
    X(System, 38, GetControllerStateWithPose, Z) \
    X(System, 39, TriggerHapticPulse, Z) \
    X(System, 40, GetButtonIdNameFromEnum, S) \
    X(System, 41, GetControllerAxisTypeNameFromEnum, S) \
    X(System, 42, IsInputAvailable, IMPL) \
    X(System, 43, IsSteamVRDrawingControllers, Z) \
    X(System, 44, ShouldApplicationPause, Z) \
    X(System, 45, ShouldApplicationReduceRenderingWork, Z) \
    X(System, 46, PerformFirmwareUpdate, Z) \
    X(System, 47, AcknowledgeQuit_Exiting, IMPL) \
    X(System, 48, GetAppContainerFilePaths, Z) \
    X(System, 49, GetRuntimeVersion, IMPL) \
    X(System, 50, SetSDKVersion, IMPL)

#define KLVR_SLOTS_Compositor(X) \
    X(Compositor, 0, SetTrackingSpace, IMPL) \
    X(Compositor, 1, GetTrackingSpace, IMPL) \
    X(Compositor, 2, WaitGetPoses, IMPL) \
    X(Compositor, 3, GetLastPoses, IMPL) \
    X(Compositor, 4, GetLastPoseForTrackedDeviceIndex, IMPL) \
    X(Compositor, 5, GetSubmitTexture, IMPL) \
    X(Compositor, 6, Submit, IMPL) \
    X(Compositor, 7, SubmitWithArrayIndex, IMPL) \
    X(Compositor, 8, ClearLastSubmittedFrame, IMPL) \
    X(Compositor, 9, PostPresentHandoff, IMPL) \
    X(Compositor, 10, GetFrameTiming, IMPL) \
    X(Compositor, 11, GetFrameTimings, IMPL) \
    X(Compositor, 12, GetFrameTimeRemaining, F) \
    X(Compositor, 13, GetCumulativeStats, Z) \
    X(Compositor, 14, FadeToColor, Z) \
    X(Compositor, 15, GetCurrentFadeColor, C) \
    X(Compositor, 16, FadeGrid, IMPL) \
    X(Compositor, 17, GetCurrentGridAlpha, IMPL) \
    X(Compositor, 18, SetSkyboxOverride, IMPL) \
    X(Compositor, 19, ClearSkyboxOverride, Z) \
    X(Compositor, 20, CompositorBringToFront, Z) \
    X(Compositor, 21, CompositorGoToBack, Z) \
    X(Compositor, 22, CompositorQuit, Z) \
    X(Compositor, 23, IsFullscreen, Z) \
    X(Compositor, 24, GetCurrentSceneFocusProcess, Z) \
    X(Compositor, 25, GetLastFrameRenderer, Z) \
    X(Compositor, 26, CanRenderScene, Z) \
    X(Compositor, 27, ShowMirrorWindow, Z) \
    X(Compositor, 28, HideMirrorWindow, Z) \
    X(Compositor, 29, IsMirrorWindowVisible, Z) \
    X(Compositor, 30, CompositorDumpImages, Z) \
    X(Compositor, 31, ShouldAppRenderWithLowResources, Z) \
    X(Compositor, 32, ForceInterleavedReprojectionOn, Z) \
    X(Compositor, 33, ForceReconnectProcess, Z) \
    X(Compositor, 34, SuspendRendering, IMPL) \
    X(Compositor, 35, GetMirrorTextureD3D11, Z) \
    X(Compositor, 36, ReleaseMirrorTextureD3D11, Z) \
    X(Compositor, 37, GetMirrorTextureGL, Z) \
    X(Compositor, 38, ReleaseSharedGLTexture, Z) \
    X(Compositor, 39, LockGLSharedTextureForAccess, Z) \
    X(Compositor, 40, UnlockGLSharedTextureForAccess, Z) \
    X(Compositor, 41, GetVulkanInstanceExtensionsRequired, Z) \
    X(Compositor, 42, GetVulkanDeviceExtensionsRequired, Z) \
    X(Compositor, 43, SetExplicitTimingMode, Z) \
    X(Compositor, 44, SubmitExplicitTimingData, Z) \
    X(Compositor, 45, IsMotionSmoothingEnabled, Z) \
    X(Compositor, 46, IsMotionSmoothingSupported, Z) \
    X(Compositor, 47, IsCurrentSceneFocusAppLoading, Z) \
    X(Compositor, 48, SetStageOverride_Async, IMPL) \
    X(Compositor, 49, ClearStageOverride, IMPL) \
    X(Compositor, 50, GetCompositorBenchmarkResults, Z) \
    X(Compositor, 51, GetLastPosePredictionIDs, IMPL) \
    X(Compositor, 52, GetPosesForFrame, IMPL)

#define KLVR_SLOTS_Input(X) \
    X(Input, 0, SetActionManifestPath, IMPL) \
    X(Input, 1, GetActionSetHandle, IMPL) \
    X(Input, 2, GetActionHandle, IMPL) \
    X(Input, 3, GetInputSourceHandle, IMPL) \
    X(Input, 4, UpdateActionState, IMPL) \
    X(Input, 5, GetDigitalActionData, IMPL) \
    X(Input, 6, GetAnalogActionData, IMPL) \
    X(Input, 7, GetPoseActionDataRelativeToNow, IMPL) \
    X(Input, 8, GetPoseActionDataForNextFrame, IMPL) \
    X(Input, 9, GetSkeletalActionData, IMPL) \
    X(Input, 10, GetDominantHand, IMPL) \
    X(Input, 11, SetDominantHand, E13) \
    X(Input, 12, GetEyeTrackingDataRelativeToNow, E13) \
    X(Input, 13, GetEyeTrackingDataForNextFrame, E13) \
    X(Input, 14, GetBoneCount, E13) \
    X(Input, 15, GetBoneHierarchy, E13) \
    X(Input, 16, GetBoneName, E13) \
    X(Input, 17, GetSkeletalReferenceTransforms, E13) \
    X(Input, 18, GetSkeletalTrackingLevel, E13) \
    X(Input, 19, GetSkeletalBoneData, E13) \
    X(Input, 20, GetSkeletalSummaryData, IMPL) \
    X(Input, 21, GetSkeletalBoneDataCompressed, E13) \
    X(Input, 22, DecompressSkeletalBoneData, E13) \
    X(Input, 23, TriggerHapticVibrationAction, IMPL) \
    X(Input, 24, GetActionOrigins, IMPL) \
    X(Input, 25, GetOriginLocalizedName, IMPL) \
    X(Input, 26, GetOriginTrackedDeviceInfo, IMPL) \
    X(Input, 27, GetActionBindingInfo, E13) \
    X(Input, 28, ShowActionOrigins, E13) \
    X(Input, 29, ShowBindingsForActionSet, E13) \
    X(Input, 30, GetComponentStateForBinding, E13) \
    X(Input, 31, IsUsingLegacyInput, IMPL) \
    X(Input, 32, OpenBindingUI, E13) \
    X(Input, 33, GetBindingVariant, IMPL)

#define KLVR_SLOTS_Chaperone(X) \
    X(Chaperone, 0, GetCalibrationState, IMPL) \
    X(Chaperone, 1, GetPlayAreaSize, Z) \
    X(Chaperone, 2, GetPlayAreaRect, IMPL) \
    X(Chaperone, 3, ReloadInfo, Z) \
    X(Chaperone, 4, SetSceneColor, Z) \
    X(Chaperone, 5, GetBoundsColor, Z) \
    X(Chaperone, 6, AreBoundsVisible, IMPL) \
    X(Chaperone, 7, ForceBoundsVisible, IMPL) \
    X(Chaperone, 8, ResetZeroPose, Z)

#define KLVR_SLOTS_Overlay(X) \
    X(Overlay, 0, FindOverlay, Z) \
    X(Overlay, 1, CreateOverlay, Z) \
    X(Overlay, 2, CreateSubviewOverlay, Z) \
    X(Overlay, 3, DestroyOverlay, Z) \
    X(Overlay, 4, GetOverlayKey, Z) \
    X(Overlay, 5, GetOverlayName, Z) \
    X(Overlay, 6, SetOverlayName, Z) \
    X(Overlay, 7, GetOverlayImageData, Z) \
    X(Overlay, 8, GetOverlayErrorNameFromEnum, S) \
    X(Overlay, 9, SetOverlayRenderingPid, Z) \
    X(Overlay, 10, GetOverlayRenderingPid, Z) \
    X(Overlay, 11, SetOverlayFlag, Z) \
    X(Overlay, 12, GetOverlayFlag, Z) \
    X(Overlay, 13, GetOverlayFlags, Z) \
    X(Overlay, 14, SetOverlayColor, Z) \
    X(Overlay, 15, GetOverlayColor, Z) \
    X(Overlay, 16, SetOverlayAlpha, Z) \
    X(Overlay, 17, GetOverlayAlpha, Z) \
    X(Overlay, 18, SetOverlayTexelAspect, Z) \
    X(Overlay, 19, GetOverlayTexelAspect, Z) \
    X(Overlay, 20, SetOverlaySortOrder, Z) \
    X(Overlay, 21, GetOverlaySortOrder, Z) \
    X(Overlay, 22, SetOverlayWidthInMeters, Z) \
    X(Overlay, 23, GetOverlayWidthInMeters, Z) \
    X(Overlay, 24, SetOverlayCurvature, Z) \
    X(Overlay, 25, GetOverlayCurvature, Z) \
    X(Overlay, 26, SetOverlayPreCurvePitch, Z) \
    X(Overlay, 27, GetOverlayPreCurvePitch, Z) \
    X(Overlay, 28, SetOverlayTextureColorSpace, Z) \
    X(Overlay, 29, GetOverlayTextureColorSpace, Z) \
    X(Overlay, 30, SetOverlayTextureBounds, Z) \
    X(Overlay, 31, GetOverlayTextureBounds, Z) \
    X(Overlay, 32, GetOverlayTransformType, Z) \
    X(Overlay, 33, SetOverlayTransformAbsolute, Z) \
    X(Overlay, 34, GetOverlayTransformAbsolute, Z) \
    X(Overlay, 35, SetOverlayTransformTrackedDeviceRelative, Z) \
    X(Overlay, 36, GetOverlayTransformTrackedDeviceRelative, Z) \
    X(Overlay, 37, SetOverlayTransformTrackedDeviceComponent, Z) \
    X(Overlay, 38, GetOverlayTransformTrackedDeviceComponent, Z) \
    X(Overlay, 39, SetOverlayTransformCursor, Z) \
    X(Overlay, 40, GetOverlayTransformCursor, Z) \
    X(Overlay, 41, SetOverlayTransformProjection, Z) \
    X(Overlay, 42, SetSubviewPosition, Z) \
    X(Overlay, 43, ShowOverlay, Z) \
    X(Overlay, 44, HideOverlay, Z) \
    X(Overlay, 45, IsOverlayVisible, Z) \
    X(Overlay, 46, GetTransformForOverlayCoordinates, Z) \
    X(Overlay, 47, WaitFrameSync, Z) \
    X(Overlay, 48, PollNextOverlayEvent, Z) \
    X(Overlay, 49, GetOverlayInputMethod, Z) \
    X(Overlay, 50, SetOverlayInputMethod, Z) \
    X(Overlay, 51, GetOverlayMouseScale, Z) \
    X(Overlay, 52, SetOverlayMouseScale, Z) \
    X(Overlay, 53, ComputeOverlayIntersection, Z) \
    X(Overlay, 54, IsHoverTargetOverlay, Z) \
    X(Overlay, 55, SetOverlayIntersectionMask, Z) \
    X(Overlay, 56, TriggerLaserMouseHapticVibration, Z) \
    X(Overlay, 57, SetOverlayCursor, Z) \
    X(Overlay, 58, SetOverlayCursorPositionOverride, Z) \
    X(Overlay, 59, ClearOverlayCursorPositionOverride, Z) \
    X(Overlay, 60, SetOverlayTexture, Z) \
    X(Overlay, 61, ClearOverlayTexture, Z) \
    X(Overlay, 62, SetOverlayRaw, Z) \
    X(Overlay, 63, SetOverlayFromFile, Z) \
    X(Overlay, 64, GetOverlayTexture, Z) \
    X(Overlay, 65, ReleaseNativeOverlayHandle, Z) \
    X(Overlay, 66, GetOverlayTextureSize, Z) \
    X(Overlay, 67, CreateDashboardOverlay, Z) \
    X(Overlay, 68, IsDashboardVisible, Z) \
    X(Overlay, 69, IsActiveDashboardOverlay, Z) \
    X(Overlay, 70, SetDashboardOverlaySceneProcess, Z) \
    X(Overlay, 71, GetDashboardOverlaySceneProcess, Z) \
    X(Overlay, 72, ShowDashboard, Z) \
    X(Overlay, 73, GetPrimaryDashboardDevice, Z) \
    X(Overlay, 74, ShowKeyboard, Z) \
    X(Overlay, 75, ShowKeyboardForOverlay, Z) \
    X(Overlay, 76, GetKeyboardText, Z) \
    X(Overlay, 77, HideKeyboard, Z) \
    X(Overlay, 78, SetKeyboardTransformAbsolute, Z) \
    X(Overlay, 79, SetKeyboardPositionForOverlay, Z) \
    X(Overlay, 80, ShowMessageOverlay, Z) \
    X(Overlay, 81, CloseMessageOverlay, Z)

#define KLVR_SLOTS_Applications(X) \
    X(Applications, 0, AddApplicationManifest, Z) \
    X(Applications, 1, RemoveApplicationManifest, Z) \
    X(Applications, 2, IsApplicationInstalled, Z) \
    X(Applications, 3, GetApplicationCount, Z) \
    X(Applications, 4, GetApplicationKeyByIndex, Z) \
    X(Applications, 5, GetApplicationKeyByProcessId, Z) \
    X(Applications, 6, LaunchApplication, Z) \
    X(Applications, 7, LaunchTemplateApplication, Z) \
    X(Applications, 8, LaunchApplicationFromMimeType, Z) \
    X(Applications, 9, LaunchDashboardOverlay, Z) \
    X(Applications, 10, CancelApplicationLaunch, Z) \
    X(Applications, 11, IdentifyApplication, Z) \
    X(Applications, 12, GetApplicationProcessId, Z) \
    X(Applications, 13, GetApplicationsErrorNameFromEnum, S) \
    X(Applications, 14, GetApplicationPropertyString, Z) \
    X(Applications, 15, GetApplicationPropertyBool, Z) \
    X(Applications, 16, GetApplicationPropertyUint64, Z) \
    X(Applications, 17, SetApplicationAutoLaunch, Z) \
    X(Applications, 18, GetApplicationAutoLaunch, Z) \
    X(Applications, 19, SetDefaultApplicationForMimeType, Z) \
    X(Applications, 20, GetDefaultApplicationForMimeType, Z) \
    X(Applications, 21, GetApplicationSupportedMimeTypes, Z) \
    X(Applications, 22, GetApplicationsThatSupportMimeType, Z) \
    X(Applications, 23, GetApplicationLaunchArguments, Z) \
    X(Applications, 24, GetStartingApplication, Z) \
    X(Applications, 25, GetSceneApplicationState, Z) \
    X(Applications, 26, PerformApplicationPrelaunchCheck, Z) \
    X(Applications, 27, GetSceneApplicationStateNameFromEnum, S) \
    X(Applications, 28, LaunchInternalProcess, Z) \
    X(Applications, 29, GetCurrentSceneProcessId, Z)

#define KLVR_SLOTS_OverlayView(X) \
    X(OverlayView, 0, AcquireOverlayView, Z) \
    X(OverlayView, 1, ReleaseOverlayView, Z) \
    X(OverlayView, 2, PostOverlayEvent, Z) \
    X(OverlayView, 3, IsViewingPermitted, Z)

#define KLVR_SLOTS_RenderModels(X) \
    X(RenderModels, 0, LoadRenderModel_Async, Z) \
    X(RenderModels, 1, FreeRenderModel, Z) \
    X(RenderModels, 2, LoadTexture_Async, Z) \
    X(RenderModels, 3, FreeTexture, Z) \
    X(RenderModels, 4, LoadTextureD3D11_Async, Z) \
    X(RenderModels, 5, LoadIntoTextureD3D11_Async, Z) \
    X(RenderModels, 6, FreeTextureD3D11, Z) \
    X(RenderModels, 7, GetRenderModelName, Z) \
    X(RenderModels, 8, GetRenderModelCount, Z) \
    X(RenderModels, 9, GetComponentCount, Z) \
    X(RenderModels, 10, GetComponentName, Z) \
    X(RenderModels, 11, GetComponentButtonMask, Z) \
    X(RenderModels, 12, GetComponentRenderModelName, Z) \
    X(RenderModels, 13, GetComponentStateForDevicePath, Z) \
    X(RenderModels, 14, GetComponentState, Z) \
    X(RenderModels, 15, RenderModelHasComponent, Z) \
    X(RenderModels, 16, GetRenderModelThumbnailURL, Z) \
    X(RenderModels, 17, GetRenderModelOriginalPath, Z) \
    X(RenderModels, 18, GetRenderModelErrorNameFromEnum, S)

#define KLVR_SLOTS_HeadsetView(X) \
    X(HeadsetView, 0, SetHeadsetViewSize, Z) \
    X(HeadsetView, 1, GetHeadsetViewSize, Z) \
    X(HeadsetView, 2, SetHeadsetViewMode, Z) \
    X(HeadsetView, 3, GetHeadsetViewMode, Z) \
    X(HeadsetView, 4, SetHeadsetViewCropped, Z) \
    X(HeadsetView, 5, GetHeadsetViewCropped, Z) \
    X(HeadsetView, 6, GetHeadsetViewAspectRatio, F) \
    X(HeadsetView, 7, SetHeadsetViewBlendRange, Z) \
    X(HeadsetView, 8, GetHeadsetViewBlendRange, Z)

#define KLVR_SLOTS_Mailbox(X) \
    X(Mailbox, 0, RegisterMailbox, IMPL) \
    X(Mailbox, 1, UnregisterMailbox, IMPL) \
    X(Mailbox, 2, SendMailbox, IMPL) \
    X(Mailbox, 3, ReadMailbox, IMPL) \
    X(Mailbox, 4, MailboxSlot4, Z) \
    X(Mailbox, 5, MailboxSlot5, Z) \
    X(Mailbox, 6, MailboxSlot6, Z) \
    X(Mailbox, 7, MailboxSlot7, Z) \
    X(Mailbox, 8, MailboxSlot8, Z) \
    X(Mailbox, 9, MailboxSlot9, Z) \
    X(Mailbox, 10, MailboxSlot10, Z) \
    X(Mailbox, 11, MailboxSlot11, Z) \
    X(Mailbox, 12, MailboxSlot12, Z) \
    X(Mailbox, 13, MailboxSlot13, Z) \
    X(Mailbox, 14, MailboxSlot14, Z) \
    X(Mailbox, 15, MailboxSlot15, Z) \
    X(Mailbox, 16, MailboxSlot16, Z) \
    X(Mailbox, 17, MailboxSlot17, Z) \
    X(Mailbox, 18, MailboxSlot18, Z) \
    X(Mailbox, 19, MailboxSlot19, Z) \
    X(Mailbox, 20, MailboxSlot20, Z) \
    X(Mailbox, 21, MailboxSlot21, Z) \
    X(Mailbox, 22, MailboxSlot22, Z) \
    X(Mailbox, 23, MailboxSlot23, Z) \
    X(Mailbox, 24, MailboxSlot24, Z) \
    X(Mailbox, 25, MailboxSlot25, Z) \
    X(Mailbox, 26, MailboxSlot26, Z) \
    X(Mailbox, 27, MailboxSlot27, Z) \
    X(Mailbox, 28, MailboxSlot28, Z) \
    X(Mailbox, 29, MailboxSlot29, Z) \
    X(Mailbox, 30, MailboxSlot30, Z) \
    X(Mailbox, 31, MailboxSlot31, Z)

#define KLVR_SLOTS_XrProto(X) \
    X(XrProto, 0, XrProtoSlot0, Z) \
    X(XrProto, 1, XrProtoSlot1, Z) \
    X(XrProto, 2, XrProtoSlot2, Z) \
    X(XrProto, 3, XrProtoSlot3, Z) \
    X(XrProto, 4, XrProtoSlot4, Z) \
    X(XrProto, 5, XrProtoSlot5, Z) \
    X(XrProto, 6, XrProtoSlot6, Z) \
    X(XrProto, 7, XrProtoSlot7, Z) \
    X(XrProto, 8, XrProtoSlot8, Z) \
    X(XrProto, 9, XrProtoSlot9, Z) \
    X(XrProto, 10, XrProtoSlot10, Z) \
    X(XrProto, 11, XrProtoSlot11, Z) \
    X(XrProto, 12, XrProtoSlot12, Z) \
    X(XrProto, 13, XrProtoSlot13, Z) \
    X(XrProto, 14, XrProtoSlot14, Z) \
    X(XrProto, 15, XrProtoSlot15, Z) \
    X(XrProto, 16, XrProtoSlot16, Z) \
    X(XrProto, 17, XrProtoSlot17, Z) \
    X(XrProto, 18, XrProtoSlot18, Z) \
    X(XrProto, 19, XrProtoSlot19, Z) \
    X(XrProto, 20, XrProtoSlot20, Z) \
    X(XrProto, 21, XrProtoSlot21, Z) \
    X(XrProto, 22, XrProtoSlot22, Z) \
    X(XrProto, 23, XrProtoSlot23, Z) \
    X(XrProto, 24, XrProtoSlot24, Z) \
    X(XrProto, 25, XrProtoSlot25, Z) \
    X(XrProto, 26, XrProtoSlot26, Z) \
    X(XrProto, 27, XrProtoSlot27, Z) \
    X(XrProto, 28, XrProtoSlot28, Z) \
    X(XrProto, 29, XrProtoSlot29, Z) \
    X(XrProto, 30, XrProtoSlot30, Z) \
    X(XrProto, 31, XrProtoSlot31, Z)

#define KLVR_SLOTS_InputInternal(X) \
    X(InputInternal, 0, InputInternalSlot0, Z) \
    X(InputInternal, 1, InputInternalSlot1, Z) \
    X(InputInternal, 2, InputInternalSlot2, Z) \
    X(InputInternal, 3, InputInternalSlot3, Z) \
    X(InputInternal, 4, InputInternalSlot4, Z) \
    X(InputInternal, 5, InputInternalSlot5, Z) \
    X(InputInternal, 6, InputInternalSlot6, Z) \
    X(InputInternal, 7, InputInternalSlot7, Z) \
    X(InputInternal, 8, InputInternalSlot8, Z) \
    X(InputInternal, 9, InputInternalSlot9, Z) \
    X(InputInternal, 10, InputInternalSlot10, Z) \
    X(InputInternal, 11, InputInternalSlot11, Z) \
    X(InputInternal, 12, InputInternalSlot12, Z) \
    X(InputInternal, 13, InputInternalSlot13, Z) \
    X(InputInternal, 14, InputInternalSlot14, Z) \
    X(InputInternal, 15, InputInternalSlot15, Z) \
    X(InputInternal, 16, InputInternalSlot16, Z) \
    X(InputInternal, 17, InputInternalSlot17, Z) \
    X(InputInternal, 18, InputInternalSlot18, Z) \
    X(InputInternal, 19, InputInternalSlot19, Z) \
    X(InputInternal, 20, InputInternalSlot20, Z) \
    X(InputInternal, 21, InputInternalSlot21, Z) \
    X(InputInternal, 22, InputInternalSlot22, Z) \
    X(InputInternal, 23, InputInternalSlot23, Z) \
    X(InputInternal, 24, InputInternalSlot24, Z) \
    X(InputInternal, 25, InputInternalSlot25, Z) \
    X(InputInternal, 26, InputInternalSlot26, Z) \
    X(InputInternal, 27, InputInternalSlot27, Z) \
    X(InputInternal, 28, InputInternalSlot28, Z) \
    X(InputInternal, 29, InputInternalSlot29, Z) \
    X(InputInternal, 30, InputInternalSlot30, Z) \
    X(InputInternal, 31, InputInternalSlot31, Z)

#endif

#!/usr/bin/env python3
"""check-touch-map.py <vivecraft-fabric.jar> <javap> [QC oculus_defaults.json]

Static check of patches 0003+0004 (Touch map) and 0005 (direct post) on the BUILT jar.
0005: javap proves the VR post pass takes its target from VRRenderer.getPostProcessTarget, that OpenXRStereoRenderer
acquires the swapchain image there and skips blitTextures in endFrame when it did, that the property fallback
(vivecraft.klepton.directpost) is read, that FrameBufferCacheVRMixin routes SwapchainLayerTexture.View to its own
framebuffer and is registered, and that SwapchainLayerTexture.close never deletes the runtime's GL name.

Touch map: Reads XRBinding.quest2Bindings() bytecode with javap,
pairs each (action, source) ldc couple, and asserts the Touch map is exactly QuestCraft's Touch layout plus
Simple Voice Chat push-to-talk on the left stick click (inside the getInputActionByName null guard).
Compared as multisets, so a source is double-bound only where QC itself double-binds it.
QC_TOUCH is QC 1.3.7 assets/vivecraft/input/oculus_defaults.json (== QC 1.2.5 XRBindings.quest2Bindings, which
writes /input/squeeze and /input/trigger shorthand). Pass the json (e.g. unzip -p the 1.21.11 Vivecraft.jar
assets/vivecraft/input/oculus_defaults.json > f.json) to re-verify the embedded table against it."""
import collections, json, re, subprocess, sys

L, R = "/user/hand/left/input/", "/user/hand/right/input/"
QC_TOUCH = [
    ("global", "vivecraft.key.ingameMenuButton", L + "menu/click"),
    ("global", "key.inventory", L + "y/click"),
    ("gui", "vivecraft.key.guiShift", L + "squeeze/value"),
    ("gui", "vivecraft.key.guiMiddleClick", R + "squeeze/value"),
    ("gui", "vivecraft.key.guiLeftClick", R + "trigger/value"),
    ("gui", "vivecraft.key.guiRightClick", R + "a/click"),
    ("gui", "vivecraft.key.guiScrollAxis", R + "thumbstick/y"),
    ("ingame", "vivecraft.key.hotbarPrev", L + "squeeze/value"),
    ("ingame", "vivecraft.key.hotbarNext", R + "squeeze/value"),
    ("ingame", "key.attack", R + "trigger/value"),
    ("ingame", "vivecraft.key.teleport", L + "x/click"),
    ("ingame", "vivecraft.key.radialMenu", R + "b/click"),
    ("ingame", "key.use", L + "trigger/value"),
    ("ingame", "vivecraft.key.freeMoveStrafe", L + "thumbstick"),
    ("ingame", "key.jump", R + "a/click"),
    ("ingame", "vivecraft.key.rotateAxis", R + "thumbstick"),
    ("ingame", "key.sneak", R + "thumbstick/click"),
    ("ingame", "vivecraft.key.teleportFallback", L + "x/click"),
    ("keyboard", "vivecraft.key.keyboardShift", L + "squeeze/value"),
    ("keyboard", "vivecraft.key.keyboardShift", R + "squeeze/value"),
    ("keyboard", "vivecraft.key.keyboardClick", L + "trigger/value"),
    ("keyboard", "vivecraft.key.keyboardClick", R + "trigger/value"),
] + [("contextual", a, h + s) for a in ("vivecraft.key.vrInteract", "vivecraft.key.climbeyGrab")
     for h in (L, R) for s in ("squeeze/value", "trigger/value")]
PTT = ("mod", "key.push_to_talk", L + "thumbstick/click")

def rows(t): return collections.Counter(f"/actions/{s}/in/{a}|{src}" for s, a, src in t)

jar, javap = sys.argv[1], sys.argv[2]
if len(sys.argv) > 3:
    prof = json.load(open(sys.argv[3]))
    assert prof["controller_paths"]["openxr"] == "/interaction_profiles/oculus/touch_controller"
    got = [(k, a["action"], src["path"]) for k, st in prof["sets"].items() for src in st.get("sources", [])
           for a in src["inputs"]]
    assert rows(got) == rows(QC_TOUCH), (rows(got) - rows(QC_TOUCH), rows(QC_TOUCH) - rows(got))
    print(f"[touch-map] embedded QC table == {sys.argv[3]} ({len(got)} rows)")

out = subprocess.run([javap, "-c", "-p", "-cp", jar, "org.vivecraft.client_vr.provider.openxr.control.XRBinding"],
                     capture_output=True, text=True, check=True).stdout
lines = out.split("quest2Bindings();", 1)[1].split("\n\n", 1)[0].splitlines()
strs = [m.group(1) for l in lines if (m := re.search(r"// String (\S+)", l))]
pairs = [(a, b) for a, b in zip(strs, strs[1:]) if a.startswith("/actions/") and b.startswith("/user/")]
for a, b in pairs: print(f"  {a:55s} {b}")
jarrows = collections.Counter(f"{a}|{b}" for a, b in pairs)
want = rows(QC_TOUCH + [PTT])
assert jarrows == want, f"jar-only {dict(jarrows - want)}  missing {dict(want - jarrows)}"
assert any("getInputActionByName" in l for l in lines) and any("ifnull" in l for l in lines), "no PTT null guard"
print(f"[touch-map] OK: {len(pairs)} bindings == QuestCraft Touch layout ({len(QC_TOUCH)}) + "
      f"key.push_to_talk -> {PTT[2]} (guarded)")

# ---- 0005: direct post (bytecode) ----
def javap_c(cls):
    return subprocess.run([javap, "-c", "-p", "-constants", "-cp", jar, cls], capture_output=True, text=True,
                          check=True).stdout
def method(out, sig):
    assert sig in out, f"no method {sig}"
    return out.split(sig, 1)[1].split("\n\n", 1)[0]
R_ = "org.vivecraft.client_vr.provider.openxr.OpenXRStereoRenderer"
oxr = javap_c(R_)
gpt = method(oxr, "public com.mojang.blaze3d.textures.GpuTextureView getPostProcessTarget(int);")
assert "acquireSwapchainImage" in gpt and "canPostDirect" in gpt and "directImage" in gpt, "getPostProcessTarget"
ef = method(oxr, "public void endFrame()")
i_dir, i_blit = ef.find("directImage"), ef.find("blitTextures")
assert 0 <= i_dir < i_blit and "xrReleaseSwapchainImage" in ef, "endFrame does not test directImage before the blit"
assert 'vivecraft.klepton.directpost' in oxr, "no -Dvivecraft.klepton.directpost fallback"
assert "SwapchainLayerTexture.wrap" in method(oxr, "private void setupOpenGL("), "setupOpenGL builds no views"
vph = method(javap_c("org.vivecraft.client_vr.render.helpers.VRPassHelper"), "public static void renderSingleView(")
assert "VRRenderer.getPostProcessTarget:(I)Lcom/mojang/blaze3d/textures/GpuTextureView;" in vph, "post target hook"
assert ("ShaderHelper.doVrPostProcess:(Lorg/vivecraft/api/client/data/RenderPass;Lcom/mojang/blaze3d/pipeline/"
        "RenderTarget;Lcom/mojang/blaze3d/textures/GpuTextureView;") in vph, "post pass not called with a view"
mx = javap_c("org.vivecraft.mixin.client_vr.blaze3d.opengl.FrameBufferCacheVRMixin")
assert "instanceof" in mx and "SwapchainLayerTexture$View" in mx and "setReturnValue" in mx, "FBO mixin"
cfg = subprocess.run(["unzip", "-p", jar, "vivecraft.mixins.json"], capture_output=True, text=True, check=True).stdout
assert "client_vr.blaze3d.opengl.FrameBufferCacheVRMixin" in cfg, "FBO mixin not registered"
slt = javap_c("org.vivecraft.client_vr.render.helpers.graphics.opengl.SwapchainLayerTexture")
assert "_deleteTexture" not in method(slt, "public void close();"), "close() deletes the swapchain image"
print("[direct-post] OK: post pass -> VRRenderer.getPostProcessTarget(view); OpenXR acquires there, endFrame skips "
      "blitTextures when it did; -Dvivecraft.klepton.directpost read; FrameBufferCacheVRMixin registered; "
      "SwapchainLayerTexture.close keeps the GL name")

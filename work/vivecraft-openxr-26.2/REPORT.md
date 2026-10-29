# M3 Stage B: Vivecraft OpenXR (mobile GLES) for Minecraft 26.2 (2026-10-12)

Result: **the jar is built, and it loads on 26.2.** On the host JVM (Fabric dev client, macOS):
- Vivecraft's mixins apply and the game reaches the title screen. This was checked with Fabric API 0.152.1 and with 0.161.0+26.2.
- With VR enabled, the mobile path runs exactly the QuestCraft call sequence up to `xrCreateInstance`. The host probe then refuses it as expected (`XR_ERROR_RUNTIME_UNAVAILABLE`, since the Mac has no runtime). After that, the "Failed to initialize VR" screen appears and nothing crashes.

Host-only work. No device, simulator, Klepton build, kl_openxr, klglfw or bake-script changes.

## 1. Provenance

The details, with re-fetch commands, are in `PROVENANCE.md`:
- **Upstream:** https://github.com/Vivecraft/VivecraftMod, branch `openxr` @ `13eafae1` ("update to 26.2 add vulkan support split rendering and swapchain…", 2026-07-27). It is cloned to `VivecraftMod/`, and our work is on local branch `kl-mobile`.
- **Reference:** QuestCraftPlusPlus/VivecraftMod `OpenXR-1.21.10` @ `0747838801a9`. Its `DeviceCompat` is byte-identical to `OpenXR-1.21.5` (the jar we ship) apart from CRLF line endings and `window.handle()`.
- **JDK:** Oracle GraalVM 25.0.4+7.1, macOS aarch64, at `work/native-image/graalvm/graalvm-jdk-25.0.4+7.1`. It sits next to the 21 build and is the same one the 26.2 native-image stage will need. **That directory's own PROVENANCE.md still needs a row for it.** I did not touch that file because it is in the other agent's area.
- **Gradle:** Gradle 9.3.0 (wrapper) with Loom 1.15.5. `GRADLE_USER_HOME` is pinned to `gradle-home/`, so nothing went into `~/.gradle`.

## 2. Patches (`patches/`, apply in order 0001 → 0005 with `git am ../patches/*.patch` on `13eafae1`)

| Patch | What it does |
|---|---|
| `0001-openxr-mobile-OpenGL-ES-path-port-of-QuestCraft-Devi.patch` | **The port.** Upstream's `DeviceCompat.Mobile` is commented out because **stock LWJGL 3.4.1 `lwjgl-openxr` has no `KHROpenGLESEnable`, `XrGraphicsBindingOpenGLESAndroidKHR`, `KHRLoaderInitAndroid`/`XrLoaderInitInfoAndroidKHR` or `KHRAndroidCreateInstance` classes**; QC shipped a custom LWJGL for them, and that fork has no 3.4.x (I checked the 3.4.1 jar: only `KHRLoaderInit` exists). So `Mobile` now writes the structs by hand from the Khronos layouts. It calls `xrGetOpenGLESGraphicsRequirementsKHR` through `XR10.xrGetInstanceProcAddr` and `JNI.callPJPI`, the same call LWJGL's generated `KHROpenGLEnable` makes. In order: `VLoader.setupAndroid()`, then `xrInitializeLoaderKHR(XrLoaderInitInfoAndroidKHR 1000089000)`, then `XR_KHR_opengl_es_enable`, then `XrInstanceCreateInfoAndroidKHR 1000008000` on the instance chain. GLES requirements (1000024003) are fetched in `checkRequirements` (before `xrCreateSession`), the session gets `XrGraphicsBindingOpenGLESAndroidKHR 1000024001` built from VLoader's EGL handles, and the swapchain images are typed `XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR` 1000024002 (same layout as the GL struct). Vulkan is refused with a message. **Selection:** `DeviceCompat.mobileRequested()` means `-Dvivecraft.openxr.mobile=true\|false` when that is set, otherwise `os.version` contains "Android" (the QC rule, so today's `-Dklepton.vr.android=true` os.version spoof keeps working). **QC's 2.5 s sleep** before loader init is now opt-in: `-Dvivecraft.openxr.mobile.loaderDelayMs=2500`. `VRSettings.stereoProviderPluginID` defaults to OPENXR on the mobile path (QC's default; upstream defaults to OPENVR). Upstream has no other `os.name`/Android/macOS gate in the OpenXR path; `detectDevice()` was the only one. |
| `0002-openxr-guard-input-lookups-on-profiles-without-a-bin.patch` | Three upstream bugs on our path. (1) `findActiveBindingControllerType` and `XRInputAction.getActiveAction` index the active profile's handle list with `activeAction` without a bounds or null check. That throws IOOBE/NPE for any action the active profile does not bind. (2) `getOriginName` returned `memUTF8` of the **length** IntBuffer, the same bug we noted on QC. (3) `getOriginControllerType` compared an origin path to the right aim action handle, so it always answered LEFT; it now reads the path string. |
| `0003-touch-jump-qc-default-ptt-left-stick.patch` | **Touch bindings (Austin, 2026-10-12).** Only `XRBinding.quest2Bindings` changes. `key.jump` moves from `/user/hand/left/input/thumbstick/click` to `/user/hand/right/input/a/click`, which is QuestCraft's Touch default: QC Vivecraft 1.2.5 `XRBindings.quest2Bindings` (the 1.21.5 jar we ship) and QC 1.3.7 `assets/vivecraft/input/oculus_defaults.json` (the 1.21.11 jar) agree. `/actions/mod/in/key.push_to_talk` goes on `/user/hand/left/input/thumbstick/click`. The name is SVC 2.6.24's `KeyEvents` registration (`new KeyMapping("key.push_to_talk", …)`); it is a mod binding, so `VRInputActionSet.fromKeyBinding` puts it in MOD. The binding is added only when `MCOpenXR.get().getInputActionByName(…) != null`, because `loadDefaultBindings` dereferences that lookup unchecked. Without SVC, nothing is bound to the left stick click. |
| `0004-touch-map-qc-layout.patch` | **The full QuestCraft Touch layout.** The orchestrator decided this from Austin's "it should be the same as it is now". It fixes the remaining rows from the §6.1 table, again in `quest2Bindings` only: menu moves from left Y to the left menu button, inventory from left X to left Y, `key.use` from right A to the left trigger (so A no longer uses *and* jumps), and teleport and teleportFallback from the left trigger to left X. `key.sneak` stays a plain press. QC 1.3.7 marks it TOGGLE, but in upstream that would be inert: `loadDefaultBindings` gives every handle the `ControllerMapping` type (BOOLEAN for clicks), not `XRBinding.actionType()`. QC 1.2.5, which our 1.21.5 build uses, has no type at all, so a plain press is "the same as now". Toggling stays the game's own Sneak: Toggle option. The result is QC's 30 Touch rows plus PTT, and other profiles are untouched. A scratch worktree on `13eafae1` with `git am` of 0001–0004 gave a tree identical to `kl-mobile` (2fb8b28a). |
| `0005-openxr-direct-post-VR-post-pass-renders-straight-int.patch` | **Direct post (2026-10-13, launch 14 candidate; GPU).** Upstream rendered each eye's VR post pass into an RGBA8 "L/R Eye" target and `glBlitFramebuffer`'d it into layer 0/1 of the array swapchain image in `endFrame`: one extra full-screen copy per eye per frame. Now `OpenXRStereoRenderer.getPostProcessTarget(eye)` (new `VRRenderer` hook, used by `VRPassHelper.renderSingleView`) acquires + waits the swapchain image at the LEFT eye's post pass and returns a blaze3d view of that image's layer; `endFrame` then only releases and submits (no gamma pass, no `blitTextures`). The view is `SwapchainLayerTexture` (a `GlTexture` around the swapchain GL name; `close()` never deletes it); `FrameBufferCacheVRMixin` hands any render pass whose only colour attachment is such a view the framebuffer `OpenGLRawTexture.link` already built with `glFramebufferTextureLayer` (blaze3d's own cache would use `glFramebufferTexture2D`, which cannot name a layer), so kl_glfb's `KL_XR_ARRAY_DIRECT` redirect still fires, exactly as for the old blit target. Off (upstream path, logged once) when the swapchain needs the gamma pass, on Vulkan, while the desktop mirror reads the eye targets (SINGLE/CROPPED/DUAL/MIXED_REALITY, renderAllPasses), and with **`-Dvivecraft.klepton.directpost=false`** (the A/B fallback). `-Dvivecraft.klepton.directpost.selftest=true` runs `DirectPostSelfTest` once in flat mode: the real post pass into a layer view vs post+blit into the other layer of a 2-layer SRGB8_ALPHA8 array, compared at 5 points. Proof and numbers: `work/perf/LAUNCH14-DIRECTPOST-REPORT.md`. |

**Build** (JDK 25, Fabric only; `local.properties` excludes Forge and NeoForge, and it is untracked and gitignored upstream):
```
cd work/vivecraft-openxr-26.2/VivecraftMod
printf 'project.build.excludeForge=true\nproject.build.excludeNeoforge=true\n' > local.properties
JAVA_HOME=…/work/native-image/graalvm/graalvm-jdk-25.0.4+7.1/Contents/Home \
GRADLE_USER_HOME=…/work/vivecraft-openxr-26.2/gradle-home ./gradlew --no-daemon :fabric:build -x test
```
The output is `VivecraftMod/build/libs/vivecraft-26.2-1.3.15-fabric.jar` (10.6 MB). With 0001+0002 it was sha256 `54d55d8b…c2da837b`; with 0001–0003 it was `51f2a8ae…4d5cff3e`; with 0001–0004 it was sha256 `ce108541…5503578f` (2026-10-12 18:44, `build-0004.log`; shipped on launches 9-13); **with 0001–0005 it is sha256 `e241a4c2…1ee5be7d`** (2026-10-13 12:34, `build-0005.log`). It nests `lwjgl-openxr-3.4.1.jar` (stock) and `lwjgl-openvr-3.3.6`; there are no openxr natives for macOS or iOS, and our shim supplies the loader. The build takes about 2 minutes cold and 8 s warm. The `Failed to parse fabric.mod.json` line is a harmless Loom warning about the templated file.

## 3. Proof (logs: `run1.log`, `run2.log`, `run3.log`; the gitignored dir)

The runs used `./gradlew :fabric:runClient` with `JAVA_TOOL_OPTIONS=-Dvivecraft.openxr.mobile=true`. Run 2 also set `-Djava.library.path=host-probe -Dorg.lwjgl.openxr.libname=host-probe/libopenxr_loader.dylib` and had `vrEnabled=true` in `runs/client/config/vivecraft-client-config.json`.

```
run3  (FabricLoader) Loading 52 mods:  - fabric-api 0.161.0+26.2 … - vivecraft 26.2-1.3.15
run1  (FabricLoader/Mixin) Compatibility level set to JAVA_25
      (VivecraftMixin) Vivecraft: applying 'sodium' fixes …
      (Minecraft) Backend library: LWJGL version 3.4.1-snapshot
      (Minecraft) Using graphics backend OpenGL, using drivers: 4.1 Metal - 91.7
      (Minecraft) Sound engine started … (Vivecraft) Vivecraft: Optifine not detected      <- title screen
      runs/client/config: "stereoProviderPluginID": "OPENXR"  (mobile default applied)
run2  (Vivecraft) Vivecraft: OpenXR running on Platform: Mac OS X, version: 27.2
      (Vivecraft) Vivecraft: OpenXR using the mobile OpenGL ES path (-Dvivecraft.openxr.mobile)
      [vloader-stub] setupAndroid (no-op) / getDalvikVM -> 0x16db30020 / getDalvikActivity -> 0x16db30060
      [xr-probe] xrGetInstanceProcAddr(NULL, xrCreateInstance | xrEnumerateInstanceExtensionProperties |
                 xrEnumerateApiLayerProperties | xrInitializeLoaderKHR) -> non-NULL
      [xr-probe] xrInitializeLoaderKHR(type=1000089000 LOADER_INIT_INFO_ANDROID_KHR, applicationVM=0x16db30020, …) -> 0
      [xr-probe] xrEnumerateInstanceExtensionProperties two-call -> 4 exts
      [xr-probe] xrCreateInstance: app='Vivecraft' v1 apiVersion=1.0.40 layers=0 exts=1
      [xr-probe]     enabled ext XR_KHR_opengl_es_enable
      [xr-probe]     next: XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR applicationVM=0x16db30020 …
      [xr-probe]   -> -51 (XR_ERROR_RUNTIME_UNAVAILABLE)
      (Vivecraft) Vivecraft: Failed to initialize VR: … XR method returned: XR_ERROR_RUNTIME_UNAVAILABLE / VR provider: OpenXR
      stack shows Minecraft.handler$zhk000$vivecraft$toggleVRState  (mixin applied); destroyVR clean, no NPE
```
**Runs 4 and 5 (after 0003, then after 0004; same dev client, Fabric API 0.161.0+26.2, `vrEnabled=false`):**
```
run5  (FabricLoader) Loading 52 mods: … - vivecraft 26.2-1.3.15
      (FabricLoader/Mixin) Compatibility level set to JAVA_25
      (VivecraftMixin) Vivecraft: applying 'sodium' fixes
      (Minecraft) Backend library: LWJGL version 3.4.1-snapshot
      (Minecraft) Sound engine started
      (Vivecraft) Vivecraft: Optifine not detected      <- title screen; no ERROR lines (run 4 identical)
```
Run 5 also logs about 1,970 `Missing sound for event` warnings. They follow a host audio device switch ("Current audio device has disapeared!" and then an OpenAL restart on Studio Display Speakers). That is Mac audio noise, not Vivecraft; run 4 had none.

The Touch map itself is only built after `xrCreateInstance`, which the host cannot reach. The committed static check `check-touch-map.py` therefore reads `XRBinding.quest2Bindings()` from the **built jar** with `javap -c`. It asserts that the (action, source) multiset equals QuestCraft's Touch layout plus PTT, so a source can be double-bound only where QC double-binds it. It also checks for the null guard. The embedded QC table is checked against QC 1.3.7's `oculus_defaults.json`:
```
python3 -I check-touch-map.py VivecraftMod/build/libs/vivecraft-26.2-1.3.15-fabric.jar <graalvm-25>/bin/javap qc137_oculus.json
[touch-map] embedded QC table == …/qc137_oculus.json (30 rows)
  /actions/global/in/vivecraft.key.ingameMenuButton       /user/hand/left/input/menu/click
  /actions/global/in/key.inventory                        /user/hand/left/input/y/click
  /actions/ingame/in/vivecraft.key.teleport               /user/hand/left/input/x/click
  /actions/ingame/in/key.use                              /user/hand/left/input/trigger/value
  /actions/ingame/in/vivecraft.key.teleportFallback       /user/hand/left/input/x/click
  /actions/ingame/in/key.jump                             /user/hand/right/input/a/click
  /actions/mod/in/key.push_to_talk                        /user/hand/left/input/thumbstick/click
  …
[touch-map] OK: 31 bindings == QuestCraft Touch layout (30) + key.push_to_talk -> /user/hand/left/input/thumbstick/click (guarded)
[direct-post] OK: post pass -> VRRenderer.getPostProcessTarget(view); OpenXR acquires there, endFrame skips blitTextures when it did; ...   (0005, same script)
```

That is contract points 2–6 of `vivecraft/notes/openxr-contract.md`, call for call, now with stock LWJGL 3.4.1. Everything after `xrCreateInstance` (session, swapchain, input) is not exercised on the host.

**Caveat:** the runs are Loom's dev environment (classes plus the same Mojang-named 26.2 client), not the remapped jar dropped into a plain instance. 26.x is unobfuscated, so the bytecode is the same. The first bake will be the true jar-in-instance test.

## 4. kl_openxr needs (runtime work for the 26.2 stage; nothing changed this round)

| Need | Why | Severity |
|---|---|---|
| **`KLXR_ACTION_MAX` 256 → ≥1024** | Upstream input is rewritten. It creates one action per physical source × profile × action set: 8 sets × (Touch 32 + Index 42 + Vive 20 + simple 4 = 94 after comments), plus 4 poses and 2 haptics, which is **758 actions**. Cosmos and Pico would add 56/set if their extensions were listed; they are not. At 256, `xrCreateAction` hits LIMIT_REACHED part-way through the third set. The guest then puts a NULL action into a suggestion, and the whole profile fails (HANDLE_INVALID). The path count is about 75, so the 512 pool is fine. | **Blocker** for input |
| `xrGetCurrentInteractionProfile` must keep answering the Touch path (`KL_XR_PROFILE` on, which is the default) | Upstream keys every input read on `activeController[hand] = getString(xrGetCurrentInteractionProfile(hand))`, called twice per frame, and `XRInputAction.getHandle(profile)`. XR_NULL_PATH (`KL_XR_PROFILE=0`) would mean **no input at all**. Already served. | must not regress |
| `xrPathToString` per frame (2×) | `getString` of the profile path | served |
| **One swapchain, `arraySize=2`**, `imageArrayIndex` 0/1, usage `COLOR_ATTACHMENT \| TRANSFER_DST` | Upstream splits rendering: each eye renders to a normal RenderTarget ("L Eye"/"R Eye") and is then blitted (`glBlitFramebuffer`, GL_NEAREST, through Mojang's DSA layer) into one layer of a single GL_TEXTURE_2D_ARRAY swapchain. QC used 2 swapchains with arraySize 1. kl_openxr has the array path (`KLXR_GL_TEXTURE_2D_ARRAY`, used by Unity multiview guests), but the Vivecraft-tuned per-layer composite, unwarp grid and rate-map attach have only ever seen two 2D swapchains. Verify on the first device frame. | verify |
| Acquire, Wait and Release now all happen in `endFrame` (between Begin and End) | Closes the old "frame order" open item | none |
| Bindings use leaf paths only: `/input/trigger/value`, `/input/squeeze/value` (FLOAT), `/input/thumbstick` (VEC2), `/input/thumbstick/x\|y` (FLOAT), `*/touch`, `*/click`, `/input/thumbrest/touch` | All are in `g_xr_sources` except thumbrest, which stays unbound by design. Upstream thresholds analog values itself (0.5), so `KL_XR_PATH_SHORTHAND` and `KL_XR_BOOL_THRESHOLD` become no-ops for this guest. **`KL_XR_ACTIVE_SETS` is still essential**, because every set has its own copy of each physical action. The "two bindings per hand for one action" M2 item is gone: each action now has exactly one source. | none |
| Profiles suggested: `oculus/touch_controller`, `valve/index_controller`, `htc/vive_controller`, `khr/simple_controller` (HashSet order) | kl_openxr must still pick Touch as `g_active_profile` when Index/simple are also suggested. Check the "active profile" log line. | verify |
| Per-frame call volume | For every bound handle per hand: `xrGetActionState*`, plus `xrEnumerateBoundSourcesForAction` twice per boolean read and once per float/vec2 read. That is a few hundred calls per frame, so they must stay O(1). | perf check |
| `xrEnumerateApiLayerProperties`, `xrCreateDebugUtilsMessengerEXT` (upcall) | **Not called at runtime.** Both sit behind `MCOpenXR.USE_VALIDATION = false`, a compile-time constant. The M3 report expected them; they can be dropped from the list. | none |
| `xrEnumerateDisplayRefreshRatesFB` / `xrRequestDisplayRefreshRateFB` | Now gated on `session.getCapabilities().XR_FB_display_refresh_rate`, so **trap R is fixed upstream**. `KL_XR_REFRESH_EXT=0` is safe again. | none |
| `xrGetOpenGLESGraphicsRequirementsKHR`, `xrInitializeLoaderKHR`, Android instance chain | Same as QC; already served | none |

## 5. Retargets for the 26.2 bake (native-image stage)

- **`Target_SvcPttBinding`** targets `org.vivecraft.client_vr.provider.openxr.XRBindings`, which is **gone**. The replacement is `…openxr.control.XRBinding`, a record `(VRInputActionSet actionSet, String key, String controller, ActionType actionType)`, whose `public static HashSet<XRBinding> getBinding(String headset)` is the substitution point. `…openxr.control.ControllerMapping` maps each profile's physical sources to `ActionType`. `VRInputAction` is now `provider.control.InputAction` / `openxr.control.XRInputAction`; the `handle` field is gone. Action names are `"/actions/mod/in/" + keyMapping.getName()`. **Conflict:** upstream's Touch map binds `key.jump` to `/user/hand/left/input/thumbstick/click`, the source we gave push-to-talk on 1.21.5. Free Touch sources in the upstream map include `/user/hand/left/input/menu/click` and the `*/touch` sensors. **Resolved by jar patches 0003 and 0004** (§2): the Touch map is QuestCraft's layout (jump on right A, use on the left trigger, and so on), and PTT is on the left stick click, guarded on `getInputActionByName(...) != null`. **Drop `Target_SvcPttBinding` and `mk-ptt-profile.py` for 26.2**; neither a substitution nor a profile file is needed. Upstream also has a `GuiBindings` screen. See the open questions below.
- **`Target_T4` (`MCOpenXR.destroy`)**: **drop it.** Upstream's `destroy` is already null-guarded, and run 2 showed a clean teardown after a failed init. The swapchain moved to `OpenXRStereoRenderer`, whose `destroy` is guarded too. `MCOpenXR.swapchain` no longer exists and `logError` is now static, so the old substitution would not even compile. `actionSetHandles` still exists.
- **VLoader:** `org.vivecraft.util.VLoader` is unchanged (same 6 natives, `System.loadLibrary("vloader")`), so `vloader_shim.c` and `vloader_stub.c` need no change.
- **`KlScreenWalker`:** `org.vivecraft.client.gui.settings.GuiMainVRSettings(Screen)` is unchanged. Only the Minecraft `class_437`/`class_4264` imports move to Mojang names, as the M3 report already says.
- **Foveation hooks (`kl_glfb`):**
  - The "3D Render" target name is unchanged (`VRRenderer.framebufferVrRender`, `VRTextureTarget.builder("3D Render")`).
  - `core/postprocessing_vr.fsh` is now `#version 330`, with `circle_radius` inside a std140 UBO `PostProcessUbo`. The `texCoordinates` varying and `Sampler0` are still there, so `klfb_rewrite_vr_post`'s three `strstr` keys still match, assuming ltw keeps the names.
  - **What changed:** the post pass now writes the eye RenderTargets "L Eye"/"R Eye", not the swapchain. The swapchain is then filled by a blit into layer 0/1 of the array texture.
  - For a texel-exact foveated path, the rate map must cover "3D Render", "L Eye"/"R Eye" (eye-sized, single-sampled, so the `KL_VRR_MIN_SAMPLES=1` size rule should catch them) and both swapchain layers. The blit is then physical-to-physical, as for Unity.
  - "L/R Eye Gamma" (RGBA16) targets appear only when the swapchain format is not sRGB; with kl_openxr listing SRGB8_ALPHA8 first, they are not created.
- **Vulkan reachability:** `GraphicsHelper.getHelper()` references `VulkanDevice` and `VulkanHelper`, and `OpenXRStereoRenderer` imports `VK10`/`VulkanRawTexture`. Native-image will pull in LWJGL Vulkan. Substitute `GraphicsHelper.getHelper` to always return `OpenGLHelper`, next to the Mojang Vulkan-backend substitution the M3 report proposes. The jar does not nest lwjgl-vulkan; Minecraft 26.2 ships it.
- **Launch args:** add `-Dvivecraft.openxr.mobile=true` (`kl_app.c:2506`, next to `-Dklepton.vr.android=true`). Once nothing else needs the os.version spoof, the spoof can go.

## 6. Open issues

1. **PTT source: RESOLVED (Austin, 2026-10-12).** The left stick click is a bad jump button. Jump keeps its QuestCraft button (right A, where QC 1.2.5 and QC 1.3.7 agree), and push-to-talk stays on the left stick click. Both ship as patch 0003. Patch 0004 then made the rest of the Touch map match QC as well (orchestrator, on "the same as it is now"), so A no longer doubles as use.

   **Touch defaults, QC vs upstream `openxr@13eafae1`: RESOLVED, QC layout (0003 + 0004).** QC means 1.2.5 `XRBindings` = 1.3.7 `oculus_defaults.json`. 1.2.5 writes the shorthand `/input/squeeze` and `/input/trigger`; 1.3.7 writes `/value`, as upstream does. The gui, keyboard and contextual sets, hotbar, attack, radial, strafe, rotate and sneak sources are identical.

   | Action (set) | QuestCraft = ours after 0004 | Upstream `13eafae1` |
   |---|---|---|
   | `key.use` (ingame) | left trigger | right A |
   | `vivecraft.key.teleport` (ingame) | left X | left trigger |
   | `vivecraft.key.teleportFallback` (ingame) | left X | left trigger |
   | `key.inventory` (global) | left Y | left X |
   | `vivecraft.key.ingameMenuButton` (global) | left menu | left Y |
   | `key.sneak` (ingame) | right stick click; TOGGLE in 1.3.7, plain in 1.2.5. **Ours: plain press** (a binding-level TOGGLE is inert upstream, see 0004; the game's Sneak: Toggle option applies) | right stick click, press |
   | `key.jump` (ingame) | right A (0003) | left stick click |
   | `key.push_to_talk` (mod) | ours: left stick click (0003, SVC only); QC's jar has none, and our 1.21.5 substitution and 1.21.11 profile used the same source | none |

   All 5 differing rows now match QC. The only deliberate deviations are PTT and the sneak toggle (plain press, as on 1.21.5). One risk to check on device: the menu is on `/user/hand/left/input/menu/click` again. QC 1.2.5 used the same path on 1.21.5, so kl_openxr already serves it.
2. `KLXR_ACTION_MAX` is a hard blocker for input on the first 26.2 device run (§4).
3. Only the host dev-env run has been done. The real jar-in-instance check comes with the 26.2 re-bake, along with the GraalVM 25 / LWJGL 3.4.1 `:unsafe` toolchain swap (Stage B's next step).
4. Upstream `openxr` is an **unreleased** branch, 53 commits ahead of and 18 behind `Multiloader-26.2`. Expect upstream input/render churn. Re-apply with `git am patches/*.patch`; 0001 touches only `DeviceCompat` and one `VRSettings` line.
5. Upstream input quirk, left as is: `XRInputAction.activeAction == 0` means "none", so the first handle of every action never reports as the active origin. This is cosmetic (origin names and haptics routing).

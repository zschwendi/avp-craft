#!/usr/bin/env python3
# check-subst.py <cpindex.json>  (from scripts/t8/cpscan.py on an instance's t4-ni.cp)
# Checks that every substitution/alias target and every intermediary id the reflection generator keys on exists in
# this instance's classpath (what native-image would reject at build time, found without a build). M3 stage A.
import json, sys
I = json.load(open(sys.argv[1]))
def MANUAL_CLASSES():
    import os
    return [e['name'].replace('.', '/') for e in json.load(open(os.path.dirname(os.path.abspath(__file__)) + '/../t8/reflect-extra-manual.json'))]
# (class, member, descriptor or None=any, kind) kind: m=method f=field c=class-only
T = [
 ('net/minecraft/class_6417', 'method_37461', ('(J)V', '(Lnet/minecraft/class_1041;)V'), 'm', 'Target_T3 MacosUtil.exitNativeFullscreen (long on 1.21.5, Window on 1.21.11: subst-1.21.11/Target_T3.java)'),
 ('net/minecraft/class_6417', 'method_54101', ('(J)V', '(Lnet/minecraft/class_1041;)V'), 'm', 'Target_T3 MacosUtil.clearResizableBit'),
 ('net/minecraft/class_6417', 'method_41718', '(Lnet/minecraft/class_7367;)V', 'm', 'Target_T3 MacosUtil.loadIcon'),
 ('com/mojang/blaze3d/platform/GLX', '_getCpuInfo', '()Ljava/lang/String;', 'm', 'Target_T3 GLX._getCpuInfo'),
 ('net/minecraft/class_6396', 'method_68673', '()V', 'm', 'Target_T3 SystemReport lambda (putHardware)'),
 ('net/minecraft/class_1008', 'method_4227', '(IZLjava/util/Set;)Lnet/minecraft/class_1008;', 'm', 'Target_T3 GlDebug.enableDebugCallback'),
 ('net/minecraft/class_6498', 'method_37961', '()Ljava/util/List;', 'm', 'Target_T3 NativeModuleLister.listModules'),
 ('com/mojang/text2speech/Narrator', 'getNarrator', None, 'm', 'Target_T3 Narrator'),
 ('org/lwjgl/util/tinyfd/TinyFileDialogs', 'tinyfd_messageBox', None, 'm', 'Target_T3 tinyfd'),
 ('net/minecraft/server/MinecraftServer', 'method_49384', '()Ljava/util/Optional;', 'm', 'Target_MinecraftServer_Int loadStatusIcon'),
 ('net/fabricmc/loader/impl/util/UrlUtil', 'getCodeSource', None, 'm', 'Target_UrlUtil'),
 ('org/vivecraft/client_vr/provider/openxr/MCOpenXR', 'destroy', '()V', 'm', 'Target_T4 MCOpenXR.destroy'),
 ('org/vivecraft/client_vr/provider/openxr/MCOpenXR', 'actionSetHandles', 'Ljava/util/Map;', 'f', 'Target_T4 @Alias actionSetHandles'),
 ('org/vivecraft/client_vr/provider/openxr/MCOpenXR', 'logError', '(ILjava/lang/String;[Ljava/lang/String;)V', 'm', 'Target_T4 @Alias logError'),
 ('net/caffeinemc/mods/sodium/client/render/vertex/serializers/VertexSerializerRegistryImpl', 'createSerializer',
  '(Lcom/mojang/blaze3d/vertex/VertexFormat;Lcom/mojang/blaze3d/vertex/VertexFormat;)Lnet/caffeinemc/mods/sodium/api/vertex/serializer/VertexSerializer;', 'm', 'Target_SodiumSerializers'),
 ('de/maxhenkel/voicechat/voice/client/microphone/MicrophoneManager', 'fallback', 'Z', 'f', 'Target_SvcMicrophone @Alias fallback'),
 ('de/maxhenkel/voicechat/voice/client/microphone/MicrophoneManager', 'createJavaMicrophone', None, 'm', 'Target_SvcMicrophone @Alias'),
 ('de/maxhenkel/voicechat/voice/client/microphone/MicrophoneManager', 'createALMicrophone', None, 'm', 'Target_SvcMicrophone @Alias'),
 ('de/maxhenkel/voicechat/voice/client/microphone/MicrophoneManager', 'createMicrophone', None, 'm', 'Target_SvcMicrophone @Substitute'),
 ('de/maxhenkel/voicechat/voice/client/microphone/MicrophoneManager', 'deviceNames', None, 'm', 'Target_SvcMicrophone @Substitute'),
 ('de/maxhenkel/opus4j/LibraryLoader', 'load', '(Ljava/lang/String;)V', 'm', 'Target_SvcNatives opus4j'),
 ('de/maxhenkel/rnnoise4j/LibraryLoader', 'load', '(Ljava/lang/String;)V', 'm', 'Target_SvcNatives rnnoise4j'),
 ('de/maxhenkel/speex4j/LibraryLoader', 'load', '(Ljava/lang/String;)V', 'm', 'Target_SvcNatives speex4j'),
 ('de/maxhenkel/lame4j/LibraryLoader', 'load', '(Ljava/lang/String;)V', 'm', 'Target_SvcNatives lame4j'),
 ('de/maxhenkel/nativeutils/LibraryLoader', 'load', None, 'm', 'Target_SvcNatives 5th loader (2.6.24, not yet substituted)'),
 ('net/minecraft/class_437', None, None, 'c', 'gen-reflect-extra SCREEN (Screen)'),
 ('net/minecraft/class_4970', 'method_55766', '(Lnet/minecraft/class_2680;Lnet/minecraft/class_1937;Lnet/minecraft/class_2338;Lnet/minecraft/class_1657;Lnet/minecraft/class_3965;)Lnet/minecraft/class_1269;', 'm', 'gen-reflect-extra useWithoutItem'),
]
# Push-to-talk on the left stick click: EITHER route must resolve (route A on QC 1.3.6/1.21.5, route B on 1.3.7+/1.21.11).
#   A = Target_SvcPttBinding (XRBindings.getBinding), B = data only: stage_native.sh + scripts/m3a/mk-ptt-profile.py
#   write input/profiles/Custom_Bindings.json, read by MCOpenXR.loadBindings via BindingProfile.getCurrentProfile.
PTT = {
 'A': [('org/vivecraft/client_vr/provider/openxr/XRBindings', 'getBinding', None, 'm', 'Target_SvcPttBinding XRBindings.getBinding')],
 'B': [('org/vivecraft/client_vr/provider/control/BindingProfile', 'getCurrentProfile', '()Lorg/vivecraft/client_vr/provider/control/BindingProfile;', 'm', 'BindingProfile.getCurrentProfile'),
       ('org/vivecraft/client_vr/provider/control/BindingProfile', 'GSON', 'Lcom/google/gson/Gson;', 'f', 'BindingProfile.GSON'),
       ('org/vivecraft/client_vr/settings/VRSettings', 'currentBindingProfile', 'Ljava/lang/String;', 'f', 'VRSettings.currentBindingProfile'),
       ('org/vivecraft/client_vr/provider/openxr/MCOpenXR', 'loadBindings', '()V', 'm', 'MCOpenXR.loadBindings'),
       ('org/vivecraft/client_vr/provider/control/Source', 'path', None, 'm', 'record Source'),
       ('org/vivecraft/client_vr/provider/control/ActionSet', 'sources', None, 'm', 'record ActionSet')],
}
# 26.x (M3 stage C): unobfuscated Mojang names, our Vivecraft 26.2 OpenXR jar, SVC 2.6.24, Sodium 0.9.2. Selected when
# the index has no intermediary classes (net/minecraft/class_*). Target_T4 and Target_SvcPttBinding are dropped for 26.x
# (scripts/inst.sh SUBST_SKIP); PTT is inside the jar's Touch map (patch 0003) -> route C below.
MOJ = not any(c.startswith('net/minecraft/class_') for c in I)
if MOJ:
    VSR = ('net/caffeinemc/mods/sodium/client/render/vertex/serializers/VertexSerializerRegistryImpl', 'createSerializer',
           '(Lcom/mojang/blaze3d/vertex/VertexFormat;Lcom/mojang/blaze3d/vertex/VertexFormat;)Lnet/caffeinemc/mods/sodium/api/vertex/serializer/VertexSerializer;')
    MM = 'de/maxhenkel/voicechat/voice/client/microphone/MicrophoneManager'
    T = [
     ('com/mojang/blaze3d/platform/MacosUtil', 'exitNativeFullscreen', '(Lcom/mojang/blaze3d/platform/Window;)V', 'm', 'Target_T3 MacosUtil.exitNativeFullscreen (subst-26.2)'),
     ('com/mojang/blaze3d/platform/MacosUtil', 'clearResizableBit', '(Lcom/mojang/blaze3d/platform/Window;)V', 'm', 'Target_T3 MacosUtil.clearResizableBit'),
     ('com/mojang/blaze3d/platform/MacosUtil', 'loadIcon', '(Lnet/minecraft/server/packs/resources/IoSupplier;)V', 'm', 'Target_T3 MacosUtil.loadIcon'),
     ('com/mojang/blaze3d/platform/GLX', '_getCpuInfo', '()Ljava/lang/String;', 'm', 'Target_T3 GLX._getCpuInfo'),
     ('net/minecraft/SystemReport', 'lambda$new$6', '()V', 'm', 'Target_T3 SystemReport lambda$new$6 (putHardware(new SystemInfo()))'),
     ('net/minecraft/SystemReport', 'lambda$new$7', '()V', 'm', 'Target_T3 SystemReport lambda$new$7 (putSoftware(new SystemInfo()), new in 26.x)'),
     ('com/mojang/blaze3d/opengl/GlDebug', 'enableDebugCallback', '(IZLjava/util/Set;)Lcom/mojang/blaze3d/opengl/GlDebug;', 'm', 'Target_T3 GlDebug.enableDebugCallback'),
     ('net/minecraft/util/NativeModuleLister', 'listModules', '()Ljava/util/List;', 'm', 'Target_T3 NativeModuleLister.listModules'),
     ('com/mojang/text2speech/Narrator', 'getNarrator', None, 'm', 'Target_T3 Narrator'),
     ('org/lwjgl/util/tinyfd/TinyFileDialogs', 'tinyfd_messageBox', None, 'm', 'Target_T3 tinyfd'),
     ('net/minecraft/server/MinecraftServer', 'loadStatusIcon', '()Ljava/util/Optional;', 'm', 'Target_MinecraftServer_Int loadStatusIcon (subst-fabric-src-26.2)'),
     ('net/fabricmc/loader/impl/util/UrlUtil', 'getCodeSource', None, 'm', 'Target_UrlUtil'),
     VSR + ('m', 'Target_SodiumSerializers (subst-26.2: name-keyed VertexFormat)'),
     ('com/mojang/blaze3d/vertex/VertexFormat', 'getElement', '(Ljava/lang/String;)Lcom/mojang/blaze3d/vertex/VertexFormatElement;', 'm', 'Target_SodiumSerializers VertexFormat.getElement'),
     ('com/mojang/blaze3d/GpuFormat', 'blockSize', '()I', 'm', 'Target_SodiumSerializers GpuFormat.blockSize'),
     (MM, 'fallback', 'Z', 'f', 'Target_SvcMicrophone @Alias fallback'),
     (MM, 'createJavaMicrophone', None, 'm', 'Target_SvcMicrophone @Alias'),
     (MM, 'createALMicrophone', None, 'm', 'Target_SvcMicrophone @Alias'),
     (MM, 'createMicrophone', None, 'm', 'Target_SvcMicrophone @Substitute'),
     (MM, 'deviceNames', None, 'm', 'Target_SvcMicrophone @Substitute'),
     (MM, 'canUseOpenAL', '()Z', 'm', 'Target_SvcMicrophoneJna canUseOpenAL (JNA Platform.isMac)'),
     (MM, 'shouldForceJavaImplementation', '()Z', 'm', 'Target_SvcMicrophoneJna shouldForceJavaImplementation'),
     ('de/maxhenkel/voicechat/macos/VersionCheck', 'isMacOSNativeCompatible', '()Z', 'm', 'Target_SvcVersionCheck (launch 8 codec gate)'),
     ('de/maxhenkel/opus4j/LibraryLoader', 'load', '(Ljava/lang/String;)V', 'm', 'Target_SvcNatives opus4j'),
     ('de/maxhenkel/rnnoise4j/LibraryLoader', 'load', '(Ljava/lang/String;)V', 'm', 'Target_SvcNatives rnnoise4j'),
     ('de/maxhenkel/speex4j/LibraryLoader', 'load', '(Ljava/lang/String;)V', 'm', 'Target_SvcNatives speex4j'),
     ('de/maxhenkel/lame4j/LibraryLoader', 'load', '(Ljava/lang/String;)V', 'm', 'Target_SvcNatives lame4j'),
     ('net/minecraft/client/PreferredGraphicsApi', 'getBackendsToTry', '()[Lcom/mojang/blaze3d/systems/GpuBackend;', 'm', 'Target_Vulkan PreferredGraphicsApi.getBackendsToTry'),
     ('com/mojang/blaze3d/vulkan/VulkanBackend', 'checkBackendAvailable', '()Lcom/mojang/blaze3d/systems/BackendCreationException;', 'm', 'Target_Vulkan VulkanBackend.checkBackendAvailable'),
     ('org/vivecraft/client_vr/render/helpers/graphics/GraphicsHelper', 'getHelper', '()Lorg/vivecraft/client_vr/render/helpers/graphics/GraphicsHelper;', 'm', 'Target_Vulkan Vivecraft GraphicsHelper.getHelper'),
     ('org/vivecraft/client_vr/render/helpers/graphics/opengl/OpenGLHelper', '<init>', '()V', 'm', 'Target_Vulkan OpenGLHelper()'),
     ('net/minecraft/client/gui/screens/Screen', None, None, 'c', 'gen-reflect-extra SCREEN (Screen)'),
     ('net/minecraft/world/level/block/state/BlockBehaviour', 'useWithoutItem', '(Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/Level;Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/entity/player/Player;Lnet/minecraft/world/phys/BlockHitResult;)Lnet/minecraft/world/InteractionResult;', 'm', 'gen-reflect-extra useWithoutItem'),
     ('org/vivecraft/client/gui/settings/GuiMainVRSettings', '<init>', '(Lnet/minecraft/client/gui/screens/Screen;)V', 'm', 'KlScreenWalker GuiMainVRSettings(Screen)'),
     ('net/minecraft/client/gui/Gui', 'setScreen', '(Lnet/minecraft/client/gui/screens/Screen;)V', 'm', 'KlScreenWalker gui.setScreen'),
     ('net/minecraft/client/gui/screens/options/OptionsScreen', '<init>', '(Lnet/minecraft/client/gui/screens/Screen;Lnet/minecraft/client/Options;Z)V', 'm', 'KlScreenWalker OptionsScreen(Screen,Options,boolean)'),
     ('org/vivecraft/client_vr/provider/openxr/DeviceCompat', 'mobileRequested', None, 'm', 'our jar: patch 0001 (-Dvivecraft.openxr.mobile)'),
    ]
    PTT = {'C': [('org/vivecraft/client_vr/provider/openxr/control/XRBinding', 'quest2Bindings', None, 'm', 'our jar: XRBinding.quest2Bindings (patch 0003 PTT; check-touch-map.py checks the rows)')]}
    for m in MANUAL_CLASSES():
        T.append((m, None, None, 'c', 'reflect-extra-manual.json class'))
def check(c, m, d, k):
    e = I.get(c)
    if e is None: return False, 'CLASS MISSING'
    if k == 'c': return True, ''
    mem = e['methods'] if k == 'm' else e['fields']
    hits = [x for x in mem if x[0] == m]
    ok = any(d is None or x[1] == d or (isinstance(d, tuple) and x[1] in d) for x in hits)
    return ok, '' if ok else ('descs: ' + ', '.join(x[1] for x in hits) if hits else 'MEMBER MISSING')
bad = 0
routes = {r: all(check(c, m, d, k)[0] for c, m, d, k, _ in rows) for r, rows in PTT.items()}
ok = any(routes.values())
bad += not ok
print(('OK  ' if ok else 'BAD ') + 'SVC push-to-talk binding route: ' + (', '.join(
    {'A': 'A (XRBindings substitution)', 'B': 'B (Custom_Bindings.json profile)', 'C': 'C (built into our 26.2 jar)'}[r] for r, v in routes.items() if v) or 'NONE resolves'))
for c, m, d, k, why in T:
    e = I.get(c)
    if e is None: ok, got = False, 'CLASS MISSING'
    elif k == 'c': ok, got = True, ''
    else:
        mem = e['methods'] if k == 'm' else e['fields']
        hits = [x for x in mem if x[0] == m]
        ok = any(d is None or x[1] == d or (isinstance(d, tuple) and x[1] in d) for x in hits)
        got = '' if ok else ('descs: ' + ', '.join(x[1] for x in hits) if hits else 'MEMBER MISSING')
    bad += not ok
    print(('OK  ' if ok else 'BAD ') + why + ('' if ok else '  -> ' + c + '.' + str(m) + ' ' + got))
print(bad, 'problem(s)')

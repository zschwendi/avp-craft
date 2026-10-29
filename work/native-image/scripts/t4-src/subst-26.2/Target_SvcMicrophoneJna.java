// Simple Voice Chat 2.6.24 (26.2): MicrophoneManager.useJavaImplementation() -> shouldForceJavaImplementation() ->
// canUseOpenAL() = com.sun.jna.Platform.isMac() ... JNA is NOT in the native image (t23-ni.cp drops jna/jna-platform),
// so any caller of useJavaImplementation() (createMicrophone/deviceNames are already substituted in
// subst/Target_SvcMicrophone.java, but the voice-chat settings screens call it too) would throw NoClassDefFoundError.
// OpenAL capture is what we use (OpenAL Soft over CoreAudio RemoteIO): canUseOpenAL = true, never force Java Sound.
// Shared verbatim by subst-1.21.11/ (same SVC 2.6.24); takes effect at the next 1.21.11 image build.
import com.oracle.svm.core.annotate.Substitute;
import com.oracle.svm.core.annotate.TargetClass;

@TargetClass(className = "de.maxhenkel.voicechat.voice.client.microphone.MicrophoneManager")
final class Target_SvcMicrophoneJna {
  @Substitute public static boolean canUseOpenAL() { return true; }
  @Substitute private static boolean shouldForceJavaImplementation() { return false; }
}

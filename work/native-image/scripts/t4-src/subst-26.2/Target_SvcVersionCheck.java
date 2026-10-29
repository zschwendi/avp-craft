// Simple Voice Chat 2.6.24 (1.21.11) gates every native (Opus, RNNoise, Speex, LAME) on
// macos.VersionCheck.isMacOSNativeCompatible(): os.version parsed as a Version >= 13. SpikeT23 pins os.version to
// "Android-14 (...)" so Vivecraft takes its Mobile/OpenXR path; that string does not parse, so SVC logged
// "Skipping initialization of Opus - Unsupported macOS version" and voice chat had no codec (launch 8 sim round,
// 2026-10-12). The natives are ours (Target_SvcNatives loads them from Frameworks) and run on visionOS 26+, so the
// check is true. SVC 2.6.11 (1.21.5) has no such gate; 1.21.11 (subst-1.21.11/) and 26.2 (subst-26.2/, same SVC 2.6.24) copies.
import com.oracle.svm.core.annotate.Substitute;
import com.oracle.svm.core.annotate.TargetClass;

@TargetClass(className = "de.maxhenkel.voicechat.macos.VersionCheck")
final class Target_SvcVersionCheck {
  @Substitute
  public static boolean isMacOSNativeCompatible() { return true; }
}

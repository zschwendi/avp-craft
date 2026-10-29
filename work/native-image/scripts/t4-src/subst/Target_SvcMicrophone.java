// Simple Voice Chat microphone: always try OpenAL capture first (native-image only; vivecraft SVC round 2).
//   SVC 2.6.11 hardcodes the Java Sound microphone on macOS: VoicechatClient.initializeClient does
//   `if (Platform.isMac() && !javaMicrophoneImplementation) javaMicrophoneImplementation.set(true).save()`, so a
//   staged java_microphone_implementation=false is rewritten to true at every boot (launch 5/6). This image
//   reports os.name "Mac OS X" on visionOS, and its Java Sound has no capture line ("No line matching interface
//   TargetDataLine"). The substitution ignores that config entry: ALMicrophone (alcCaptureOpenDevice -> OpenAL
//   Soft's CoreAudio RemoteIO capture, under the app's .playAndRecord session) first, Java Sound only as the
//   fallback SVC already has. deviceNames() follows the same choice so the settings list shows OpenAL devices.
import com.oracle.svm.core.annotate.Alias;
import com.oracle.svm.core.annotate.Substitute;
import com.oracle.svm.core.annotate.TargetClass;
import de.maxhenkel.voicechat.voice.client.MicrophoneException;
import de.maxhenkel.voicechat.voice.client.microphone.ALMicrophone;
import de.maxhenkel.voicechat.voice.client.microphone.JavaxMicrophone;
import de.maxhenkel.voicechat.voice.client.microphone.Microphone;
import java.util.List;

@TargetClass(className = "de.maxhenkel.voicechat.voice.client.microphone.MicrophoneManager")
final class Target_SvcMicrophoneManager {
  @Alias private static boolean fallback;

  @Alias
  private static native Microphone createJavaMicrophone() throws MicrophoneException;

  @Alias
  private static native Microphone createALMicrophone() throws MicrophoneException;

  @Substitute
  public static Microphone createMicrophone() throws MicrophoneException {
    if (!fallback) {
      try {
        Microphone mic = createALMicrophone();
        System.err.println("[kl-svc] microphone: OpenAL capture opened");
        return mic;
      } catch (MicrophoneException e) {
        System.err.println("[kl-svc] microphone: OpenAL capture failed (" + e.getMessage()
            + "), falling back to Java Sound");
        fallback = true;
      }
    }
    return createJavaMicrophone();
  }

  @Substitute
  public static List<String> deviceNames() {
    return fallback ? JavaxMicrophone.getAllMicrophones() : ALMicrophone.getAllMicrophones();
  }
}

// Simple Voice Chat push-to-talk on the left thumbstick click (native-image only; vivecraft SVC round 2).
//   The QuestCraft OpenXR Vivecraft has no user binding UI: XRBindings.getBinding(profile) is the whole suggested
//   binding set, and its Touch set leaves /user/hand/left/input/thumbstick/click free (the right click is sneak).
//   SVC's key.push_to_talk is a mod KeyMapping, so Vivecraft already made it an action (/actions/mod/in/...,
//   name from the live MCOpenXR so the action set is never guessed). With the KeyMapping bound to a real key
//   (stage_native.sh puts key.keyboard.f24 in options.txt), Vivecraft's VRInputAction.pressKey routes the press
//   through InputSimulator -> KeyboardHandler.keyPress, which is exactly what SVC's PTTKeyHandler listens to, and
//   its InputConstants.isKeyDown check is answered by Vivecraft's InputConstantsVRMixin. The pair is only added
//   when the action exists with a live handle, so a missing SVC can never leave a null binding behind.
import com.oracle.svm.core.annotate.Alias;
import com.oracle.svm.core.annotate.Substitute;
import com.oracle.svm.core.annotate.TargetClass;
import java.util.HashSet;
import org.apache.commons.lang3.tuple.Pair;
import org.vivecraft.client_vr.provider.control.VRInputAction;
import org.vivecraft.client_vr.provider.openxr.MCOpenXR;

@TargetClass(className = "org.vivecraft.client_vr.provider.openxr.XRBindings")
final class Target_SvcXRBindings {
  @Alias private static native HashSet<Pair<String, String>> cosmosBindings();
  @Alias private static native HashSet<Pair<String, String>> quest2Bindings();
  @Alias private static native HashSet<Pair<String, String>> viveBindings();

  @Substitute
  public static HashSet<Pair<String, String>> getBinding(String headset) {
    switch (headset) {
      case "/interaction_profiles/htc/vive_cosmos_controller":
        return cosmosBindings();
      case "/interaction_profiles/oculus/touch_controller": {
        HashSet<Pair<String, String>> set = quest2Bindings();
        String ptt = KlSvcPtt.actionName();
        if (ptt != null) {
          set.add(Pair.of(ptt, "/user/hand/left/input/thumbstick/click"));
          System.err.println("[kl-svc] push-to-talk: " + ptt + " -> /user/hand/left/input/thumbstick/click");
        }
        return set;
      }
      case "/interaction_profiles/bytedance/pico4_controller":
      case "/interaction_profiles/bytedance/pico_neo3_controller":
        return quest2Bindings();
      default:
        return viveBindings();
    }
  }
}

final class KlSvcPtt {
  static String actionName() {
    try {
      MCOpenXR xr = MCOpenXR.get();
      VRInputAction a = xr == null ? null : xr.getInputAction("key.push_to_talk");
      if (a == null || a.handle == 0L) {
        System.err.println("[kl-svc] push-to-talk: no VR action for key.push_to_talk, left stick click left unbound");
        return null;
      }
      return a.name;
    } catch (Throwable t) {
      System.err.println("[kl-svc] push-to-talk: binding skipped (" + t + ")");
      return null;
    }
  }
}

/*
 * Klepton GLFW stand-in (T4), forked from QuestCraftPlusPlus/pojlib jre_lwjgl3glfw/.../GLFW.java (Pojav's Java GLFW
 * stub; LWJGL license, https://www.lwjgl.org/license). The constants block is Pojav's, verbatim.
 *
 * What changed vs the Pojav stub (see work/native-image/T4-REPORT.md):
 *  - NO LWJGL closures on the GLFW path: every glfwSet*Callback keeps the GLFW*CallbackI object in a Java field
 *    (never memAddressSafe(cbfun), never GLFW*Callback.create(cbfun)); glfwPollEvents makes ONE JNI call
 *    (KlGlfwNative.nPollEvents) that copies the native event ring into a direct buffer, then calls cb.invoke(...)
 *    in Java. The previous callback is returned only when it already was a GLFW*Callback object (so callers that
 *    free() it behave as with real GLFW); a bare lambda's predecessor is reported as null.
 *  - The native side is libklglfw (scripts/t4-src/native/klglfw.c), JNI only, no libpojavexec, no
 *    JNIInvocation function pointers (Functions.* / invokePV), no CallbackBridge natives.
 *  - android.util.ArrayMap -> HashMap/LinkedHashMap; no reflection (glfwGetKeyName is a switch, not getFields()).
 *  - Window size: the requested size unless -Dglfwstub.windowWidth/Height or KLGLFW_SIZE=WxH override it.
 *  - glfwWaitEventsTimeout really waits (native condvar, woken by new events / glfwPostEmptyEvent): Minecraft's
 *    frame limiter spins on it.
 *  - Key / mouse-button state, cursor position, input modes, clipboard: Java-side state updated from the ring.
 *  - Focus + cursor-enter are delivered once after window creation (real GLFW on a shown window does the same).
 */
package org.lwjgl.glfw;

import java.nio.*;
import java.util.*;

import javax.annotation.*;

import org.lwjgl.*;
import org.lwjgl.system.*;

import static org.lwjgl.system.Checks.*;
import static org.lwjgl.system.MemoryUtil.*;

public class GLFW
{
    /** The major version number of the GLFW library. This is incremented when the API is changed in non-compatible ways. */
    public static final int GLFW_VERSION_MAJOR = 3;

    /** The minor version number of the GLFW library. This is incremented when features are added to the API but it remains backward-compatible. */
    public static final int GLFW_VERSION_MINOR = 4;

    /** The revision number of the GLFW library. This is incremented when a bug fix release is made that does not contain any API changes. */
    public static final int GLFW_VERSION_REVISION = 0;

    /** Boolean values. */
    public static final int
    GLFW_TRUE  = 1,
    GLFW_FALSE = 0;

    /** The key or button was released. */
    public static final int GLFW_RELEASE = 0;

    /** The key or button was pressed. */
    public static final int GLFW_PRESS = 1;

    /** The key was held down until it repeated. */
    public static final int GLFW_REPEAT = 2;

    /** Joystick hat states. */
    public static final int
    GLFW_HAT_CENTERED   = 0,
    GLFW_HAT_UP         = 1,
    GLFW_HAT_RIGHT      = 2,
    GLFW_HAT_DOWN       = 4,
    GLFW_HAT_LEFT       = 8,
    GLFW_HAT_RIGHT_UP   = (GLFW_HAT_RIGHT | GLFW_HAT_UP),
    GLFW_HAT_RIGHT_DOWN = (GLFW_HAT_RIGHT | GLFW_HAT_DOWN),
    GLFW_HAT_LEFT_UP    = (GLFW_HAT_LEFT  | GLFW_HAT_UP),
    GLFW_HAT_LEFT_DOWN  = (GLFW_HAT_LEFT  | GLFW_HAT_DOWN);

    /** The unknown key. */
    public static final int GLFW_KEY_UNKNOWN = -1;

    /** Printable keys. */
    public static final int
    GLFW_KEY_SPACE         = 32,
    GLFW_KEY_APOSTROPHE    = 39,
    GLFW_KEY_COMMA         = 44,
    GLFW_KEY_MINUS         = 45,
    GLFW_KEY_PERIOD        = 46,
    GLFW_KEY_SLASH         = 47,
    GLFW_KEY_0             = 48,
    GLFW_KEY_1             = 49,
    GLFW_KEY_2             = 50,
    GLFW_KEY_3             = 51,
    GLFW_KEY_4             = 52,
    GLFW_KEY_5             = 53,
    GLFW_KEY_6             = 54,
    GLFW_KEY_7             = 55,
    GLFW_KEY_8             = 56,
    GLFW_KEY_9             = 57,
    GLFW_KEY_SEMICOLON     = 59,
    GLFW_KEY_EQUAL         = 61,
    GLFW_KEY_A             = 65,
    GLFW_KEY_B             = 66,
    GLFW_KEY_C             = 67,
    GLFW_KEY_D             = 68,
    GLFW_KEY_E             = 69,
    GLFW_KEY_F             = 70,
    GLFW_KEY_G             = 71,
    GLFW_KEY_H             = 72,
    GLFW_KEY_I             = 73,
    GLFW_KEY_J             = 74,
    GLFW_KEY_K             = 75,
    GLFW_KEY_L             = 76,
    GLFW_KEY_M             = 77,
    GLFW_KEY_N             = 78,
    GLFW_KEY_O             = 79,
    GLFW_KEY_P             = 80,
    GLFW_KEY_Q             = 81,
    GLFW_KEY_R             = 82,
    GLFW_KEY_S             = 83,
    GLFW_KEY_T             = 84,
    GLFW_KEY_U             = 85,
    GLFW_KEY_V             = 86,
    GLFW_KEY_W             = 87,
    GLFW_KEY_X             = 88,
    GLFW_KEY_Y             = 89,
    GLFW_KEY_Z             = 90,
    GLFW_KEY_LEFT_BRACKET  = 91,
    GLFW_KEY_BACKSLASH     = 92,
    GLFW_KEY_RIGHT_BRACKET = 93,
    GLFW_KEY_GRAVE_ACCENT  = 96,
    GLFW_KEY_WORLD_1       = 161,
    GLFW_KEY_WORLD_2       = 162;

    /** Function keys. */
    public static final int
    GLFW_KEY_ESCAPE        = 256,
    GLFW_KEY_ENTER         = 257,
    GLFW_KEY_TAB           = 258,
    GLFW_KEY_BACKSPACE     = 259,
    GLFW_KEY_INSERT        = 260,
    GLFW_KEY_DELETE        = 261,
    GLFW_KEY_RIGHT         = 262,
    GLFW_KEY_LEFT          = 263,
    GLFW_KEY_DOWN          = 264,
    GLFW_KEY_UP            = 265,
    GLFW_KEY_PAGE_UP       = 266,
    GLFW_KEY_PAGE_DOWN     = 267,
    GLFW_KEY_HOME          = 268,
    GLFW_KEY_END           = 269,
    GLFW_KEY_CAPS_LOCK     = 280,
    GLFW_KEY_SCROLL_LOCK   = 281,
    GLFW_KEY_NUM_LOCK      = 282,
    GLFW_KEY_PRINT_SCREEN  = 283,
    GLFW_KEY_PAUSE         = 284,
    GLFW_KEY_F1            = 290,
    GLFW_KEY_F2            = 291,
    GLFW_KEY_F3            = 292,
    GLFW_KEY_F4            = 293,
    GLFW_KEY_F5            = 294,
    GLFW_KEY_F6            = 295,
    GLFW_KEY_F7            = 296,
    GLFW_KEY_F8            = 297,
    GLFW_KEY_F9            = 298,
    GLFW_KEY_F10           = 299,
    GLFW_KEY_F11           = 300,
    GLFW_KEY_F12           = 301,
    GLFW_KEY_F13           = 302,
    GLFW_KEY_F14           = 303,
    GLFW_KEY_F15           = 304,
    GLFW_KEY_F16           = 305,
    GLFW_KEY_F17           = 306,
    GLFW_KEY_F18           = 307,
    GLFW_KEY_F19           = 308,
    GLFW_KEY_F20           = 309,
    GLFW_KEY_F21           = 310,
    GLFW_KEY_F22           = 311,
    GLFW_KEY_F23           = 312,
    GLFW_KEY_F24           = 313,
    GLFW_KEY_F25           = 314,
    GLFW_KEY_KP_0          = 320,
    GLFW_KEY_KP_1          = 321,
    GLFW_KEY_KP_2          = 322,
    GLFW_KEY_KP_3          = 323,
    GLFW_KEY_KP_4          = 324,
    GLFW_KEY_KP_5          = 325,
    GLFW_KEY_KP_6          = 326,
    GLFW_KEY_KP_7          = 327,
    GLFW_KEY_KP_8          = 328,
    GLFW_KEY_KP_9          = 329,
    GLFW_KEY_KP_DECIMAL    = 330,
    GLFW_KEY_KP_DIVIDE     = 331,
    GLFW_KEY_KP_MULTIPLY   = 332,
    GLFW_KEY_KP_SUBTRACT   = 333,
    GLFW_KEY_KP_ADD        = 334,
    GLFW_KEY_KP_ENTER      = 335,
    GLFW_KEY_KP_EQUAL      = 336,
    GLFW_KEY_LEFT_SHIFT    = 340,
    GLFW_KEY_LEFT_CONTROL  = 341,
    GLFW_KEY_LEFT_ALT      = 342,
    GLFW_KEY_LEFT_SUPER    = 343,
    GLFW_KEY_RIGHT_SHIFT   = 344,
    GLFW_KEY_RIGHT_CONTROL = 345,
    GLFW_KEY_RIGHT_ALT     = 346,
    GLFW_KEY_RIGHT_SUPER   = 347,
    GLFW_KEY_MENU          = 348,
    GLFW_KEY_LAST          = GLFW_KEY_MENU;

    /** If this bit is set one or more Shift keys were held down. */
    public static final int GLFW_MOD_SHIFT = 0x1;

    /** If this bit is set one or more Control keys were held down. */
    public static final int GLFW_MOD_CONTROL = 0x2;

    /** If this bit is set one or more Alt keys were held down. */
    public static final int GLFW_MOD_ALT = 0x4;

    /** If this bit is set one or more Super keys were held down. */
    public static final int GLFW_MOD_SUPER = 0x8;

    /** If this bit is set the Caps Lock key is enabled and the {@link #GLFW_LOCK_KEY_MODS LOCK_KEY_MODS} input mode is set. */
    public static final int GLFW_MOD_CAPS_LOCK = 0x10;

    /** If this bit is set the Num Lock key is enabled and the {@link #GLFW_LOCK_KEY_MODS LOCK_KEY_MODS} input mode is set. */
    public static final int GLFW_MOD_NUM_LOCK = 0x20;


    /** Mouse buttons. See <a target="_blank" href="http://www.glfw.org/docs/latest/input.html#input_mouse_button">mouse button input</a> for how these are used. */
    public static final int
    GLFW_MOUSE_BUTTON_1      = 0,
    GLFW_MOUSE_BUTTON_2      = 1,
    GLFW_MOUSE_BUTTON_3      = 2,
    GLFW_MOUSE_BUTTON_4      = 3,
    GLFW_MOUSE_BUTTON_5      = 4,
    GLFW_MOUSE_BUTTON_6      = 5,
    GLFW_MOUSE_BUTTON_7      = 6,
    GLFW_MOUSE_BUTTON_8      = 7,
    GLFW_MOUSE_BUTTON_LAST   = GLFW_MOUSE_BUTTON_8,
    GLFW_MOUSE_BUTTON_LEFT   = GLFW_MOUSE_BUTTON_1,
    GLFW_MOUSE_BUTTON_RIGHT  = GLFW_MOUSE_BUTTON_2,
    GLFW_MOUSE_BUTTON_MIDDLE = GLFW_MOUSE_BUTTON_3;

    /** Joysticks. See <a target="_blank" href="http://www.glfw.org/docs/latest/input.html#joystick">joystick input</a> for how these are used. */
    public static final int
    GLFW_JOYSTICK_1    = 0,
    GLFW_JOYSTICK_2    = 1,
    GLFW_JOYSTICK_3    = 2,
    GLFW_JOYSTICK_4    = 3,
    GLFW_JOYSTICK_5    = 4,
    GLFW_JOYSTICK_6    = 5,
    GLFW_JOYSTICK_7    = 6,
    GLFW_JOYSTICK_8    = 7,
    GLFW_JOYSTICK_9    = 8,
    GLFW_JOYSTICK_10   = 9,
    GLFW_JOYSTICK_11   = 10,
    GLFW_JOYSTICK_12   = 11,
    GLFW_JOYSTICK_13   = 12,
    GLFW_JOYSTICK_14   = 13,
    GLFW_JOYSTICK_15   = 14,
    GLFW_JOYSTICK_16   = 15,
    GLFW_JOYSTICK_LAST = GLFW_JOYSTICK_16;

    /** Gamepad buttons. See <a target="_blank" href="http://www.glfw.org/docs/latest/input.html#gamepad">gamepad</a> for how these are used. */
    public static final int
    GLFW_GAMEPAD_BUTTON_A            = 0,
    GLFW_GAMEPAD_BUTTON_B            = 1,
    GLFW_GAMEPAD_BUTTON_X            = 2,
    GLFW_GAMEPAD_BUTTON_Y            = 3,
    GLFW_GAMEPAD_BUTTON_LEFT_BUMPER  = 4,
    GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER = 5,
    GLFW_GAMEPAD_BUTTON_BACK         = 6,
    GLFW_GAMEPAD_BUTTON_START        = 7,
    GLFW_GAMEPAD_BUTTON_GUIDE        = 8,
    GLFW_GAMEPAD_BUTTON_LEFT_THUMB   = 9,
    GLFW_GAMEPAD_BUTTON_RIGHT_THUMB  = 10,
    GLFW_GAMEPAD_BUTTON_DPAD_UP      = 11,
    GLFW_GAMEPAD_BUTTON_DPAD_RIGHT   = 12,
    GLFW_GAMEPAD_BUTTON_DPAD_DOWN    = 13,
    GLFW_GAMEPAD_BUTTON_DPAD_LEFT    = 14,
    GLFW_GAMEPAD_BUTTON_LAST         = GLFW_GAMEPAD_BUTTON_DPAD_LEFT,
    GLFW_GAMEPAD_BUTTON_CROSS        = GLFW_GAMEPAD_BUTTON_A,
    GLFW_GAMEPAD_BUTTON_CIRCLE       = GLFW_GAMEPAD_BUTTON_B,
    GLFW_GAMEPAD_BUTTON_SQUARE       = GLFW_GAMEPAD_BUTTON_X,
    GLFW_GAMEPAD_BUTTON_TRIANGLE     = GLFW_GAMEPAD_BUTTON_Y;

    /** Gamepad axes. See <a target="_blank" href="http://www.glfw.org/docs/latest/input.html#gamepad">gamepad</a> for how these are used. */
    public static final int
    GLFW_GAMEPAD_AXIS_LEFT_X        = 0,
    GLFW_GAMEPAD_AXIS_LEFT_Y        = 1,
    GLFW_GAMEPAD_AXIS_RIGHT_X       = 2,
    GLFW_GAMEPAD_AXIS_RIGHT_Y       = 3,
    GLFW_GAMEPAD_AXIS_LEFT_TRIGGER  = 4,
    GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER = 5,
    GLFW_GAMEPAD_AXIS_LAST          = GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER;

    public static final int
    GLFW_NO_ERROR              = 0,
    GLFW_NOT_INITIALIZED       = 0x10001,
    GLFW_NO_CURRENT_CONTEXT    = 0x10002,
    GLFW_INVALID_ENUM          = 0x10003,
    GLFW_INVALID_VALUE         = 0x10004,
    GLFW_OUT_OF_MEMORY         = 0x10005,
    GLFW_API_UNAVAILABLE       = 0x10006,
    GLFW_VERSION_UNAVAILABLE   = 0x10007,
    GLFW_PLATFORM_ERROR        = 0x10008,
    GLFW_FORMAT_UNAVAILABLE    = 0x10009,
    GLFW_NO_WINDOW_CONTEXT     = 0x1000A,
    GLFW_CURSOR_UNAVAILABLE    = 0x1000B,
    GLFW_FEATURE_UNAVAILABLE   = 0x1000C,
    GLFW_FEATURE_UNIMPLEMENTED = 0x1000D;

    public static final int
    GLFW_FOCUSED                 = 0x20001,
    GLFW_ICONIFIED               = 0x20002,
    GLFW_RESIZABLE               = 0x20003,
    GLFW_VISIBLE                 = 0x20004,
    GLFW_DECORATED               = 0x20005,
    GLFW_AUTO_ICONIFY            = 0x20006,
    GLFW_FLOATING                = 0x20007,
    GLFW_MAXIMIZED               = 0x20008,
    GLFW_CENTER_CURSOR           = 0x20009,
    GLFW_TRANSPARENT_FRAMEBUFFER = 0x2000A,
    GLFW_HOVERED                 = 0x2000B,
    GLFW_FOCUS_ON_SHOW           = 0x2000C;

    /** Input options. */
    public static final int
    GLFW_CURSOR               = 0x33001,
    GLFW_STICKY_KEYS          = 0x33002,
    GLFW_STICKY_MOUSE_BUTTONS = 0x33003,
    GLFW_LOCK_KEY_MODS        = 0x33004,
    GLFW_RAW_MOUSE_MOTION     = 0x33005;

    /** Cursor state. */
    public static final int
    GLFW_CURSOR_NORMAL   = 0x34001,
    GLFW_CURSOR_HIDDEN   = 0x34002,
    GLFW_CURSOR_DISABLED = 0x34003;

    /** The regular arrow cursor shape. */
    public static final int GLFW_ARROW_CURSOR = 0x36001;

    /** The text input I-beam cursor shape. */
    public static final int GLFW_IBEAM_CURSOR = 0x36002;

    /** The crosshair cursor shape. */
    public static final int GLFW_CROSSHAIR_CURSOR = 0x36003;

    /** The pointing hand cursor shape. */
    public static final int GLFW_POINTING_HAND_CURSOR = 0x36004;

    public static final int GLFW_RESIZE_EW_CURSOR = 0x36005;
    public static final int GLFW_RESIZE_NS_CURSOR = 0x36006;
    public static final int GLFW_RESIZE_NWSE_CURSOR = 0x36007;
    public static final int GLFW_RESIZE_NESW_CURSOR = 0x36008;

    /**
     * The omni-directional resize cursor/move shape.
     *
     * <p>This is usually either a combined horizontal and vertical double-headed arrow or a grabbing hand.</p>
     */
    public static final int GLFW_RESIZE_ALL_CURSOR = 0x36009;

    public static final int GLFW_NOT_ALLOWED_CURSOR = 0x3600A;

    /** Legacy name for compatibility. */
    public static final int GLFW_HRESIZE_CURSOR = GLFW_RESIZE_EW_CURSOR;

    /** Legacy name for compatibility. */
    public static final int GLFW_VRESIZE_CURSOR = GLFW_RESIZE_NS_CURSOR;

    /** Legacy name for compatibility. */
    public static final int GLFW_HAND_CURSOR = GLFW_POINTING_HAND_CURSOR;

    /** Monitor events. */
    public static final int
    GLFW_CONNECTED    = 0x40001,
    GLFW_DISCONNECTED = 0x40002;

    /** Init hints. */
    public static final int
    GLFW_JOYSTICK_HAT_BUTTONS  = 0x50001,
    GLFW_COCOA_CHDIR_RESOURCES = 0x51001,
    GLFW_COCOA_MENUBAR         = 0x51002;

    /** Hint value for {@link #GLFW_PLATFORM PLATFORM} that enables automatic platform selection. */
    public static final int
        GLFW_ANY_PLATFORM     = 0x60000,
        GLFW_PLATFORM_WIN32   = 0x60001,
        GLFW_PLATFORM_COCOA   = 0x60002,
        GLFW_PLATFORM_WAYLAND = 0x60003,
        GLFW_PLATFORM_X11     = 0x60004,
        GLFW_PLATFORM_NULL    = 0x60005;

    /** Don't care value. */
    public static final int GLFW_DONT_CARE = -1;

    /** PixelFormat hints. */
    public static final int
    GLFW_RED_BITS         = 0x21001,
    GLFW_GREEN_BITS       = 0x21002,
    GLFW_BLUE_BITS        = 0x21003,
    GLFW_ALPHA_BITS       = 0x21004,
    GLFW_DEPTH_BITS       = 0x21005,
    GLFW_STENCIL_BITS     = 0x21006,
    GLFW_ACCUM_RED_BITS   = 0x21007,
    GLFW_ACCUM_GREEN_BITS = 0x21008,
    GLFW_ACCUM_BLUE_BITS  = 0x21009,
    GLFW_ACCUM_ALPHA_BITS = 0x2100A,
    GLFW_AUX_BUFFERS      = 0x2100B,
    GLFW_STEREO           = 0x2100C,
    GLFW_SAMPLES          = 0x2100D,
    GLFW_SRGB_CAPABLE     = 0x2100E,
    GLFW_REFRESH_RATE     = 0x2100F,
    GLFW_DOUBLEBUFFER     = 0x21010;

    public static final int
    GLFW_CLIENT_API               = 0x22001,
    GLFW_CONTEXT_VERSION_MAJOR    = 0x22002,
    GLFW_CONTEXT_VERSION_MINOR    = 0x22003,
    GLFW_CONTEXT_REVISION         = 0x22004,
    GLFW_CONTEXT_ROBUSTNESS       = 0x22005,
    GLFW_OPENGL_FORWARD_COMPAT    = 0x22006,
    GLFW_OPENGL_DEBUG_CONTEXT     = 0x22007,
    GLFW_OPENGL_PROFILE           = 0x22008,
    GLFW_CONTEXT_RELEASE_BEHAVIOR = 0x22009,
    GLFW_CONTEXT_NO_ERROR         = 0x2200A,
    GLFW_CONTEXT_CREATION_API     = 0x2200B,
    GLFW_SCALE_TO_MONITOR         = 0x2200C;

    /** Specifies whether to use full resolution framebuffers on Retina displays. This is ignored on other platforms. */
    public static final int GLFW_COCOA_RETINA_FRAMEBUFFER = 0x23001;

    /**
     * Specifies the UTF-8 encoded name to use for autosaving the window frame, or if empty disables frame autosaving for the window. This is ignored on other
     * platforms. This is set with {@link #glfwWindowHintString WindowHintString}.
     */
    public static final int GLFW_COCOA_FRAME_NAME = 0x23002;

    /**
     * Specifies whether to enable Automatic Graphics Switching, i.e. to allow the system to choose the integrated GPU for the OpenGL context and move it
     * between GPUs if necessary or whether to force it to always run on the discrete GPU. This only affects systems with both integrated and discrete GPUs.
     * This is ignored on other platforms.
     */
    public static final int GLFW_COCOA_GRAPHICS_SWITCHING = 0x23003;

    /** The desired ASCII encoded class and instance parts of the ICCCM {@code WM_CLASS} window property. These are set with {@link #glfwWindowHintString WindowHintString}. */
    public static final int
    GLFW_X11_CLASS_NAME    = 0x24001,
    GLFW_X11_INSTANCE_NAME = 0x24002;

    /**
     * Specifies whether to allow access to the window menu via the Alt+Space and Alt-and-then-Space keyboard shortcuts.
     *
     * <p>This is ignored on other platforms.</p>
     */
    public static final int GLFW_WIN32_KEYBOARD_MENU = 0x25001;

    /** Values for the {@link #GLFW_CLIENT_API CLIENT_API} hint. */
    public static final int
    GLFW_NO_API        = 0,
    GLFW_OPENGL_API    = 0x30001,
    GLFW_OPENGL_ES_API = 0x30002;

    /** Values for the {@link #GLFW_CONTEXT_ROBUSTNESS CONTEXT_ROBUSTNESS} hint. */
    public static final int
    GLFW_NO_ROBUSTNESS         = 0,
    GLFW_NO_RESET_NOTIFICATION = 0x31001,
    GLFW_LOSE_CONTEXT_ON_RESET = 0x31002;

    /** Values for the {@link #GLFW_OPENGL_PROFILE OPENGL_PROFILE} hint. */
    public static final int
    GLFW_OPENGL_ANY_PROFILE    = 0,
    GLFW_OPENGL_CORE_PROFILE   = 0x32001,
    GLFW_OPENGL_COMPAT_PROFILE = 0x32002;

    /** Values for the {@link #GLFW_CONTEXT_RELEASE_BEHAVIOR CONTEXT_RELEASE_BEHAVIOR} hint. */
    public static final int
    GLFW_ANY_RELEASE_BEHAVIOR   = 0,
    GLFW_RELEASE_BEHAVIOR_FLUSH = 0x35001,
    GLFW_RELEASE_BEHAVIOR_NONE  = 0x35002;

    /** Values for the {@link #GLFW_CONTEXT_CREATION_API CONTEXT_CREATION_API} hint. */
    public static final int
    GLFW_NATIVE_CONTEXT_API = 0x36001,
    GLFW_EGL_CONTEXT_API    = 0x36002,
    GLFW_OSMESA_CONTEXT_API = 0x36003;

    // =================================================================================================================
    // Klepton stand-in implementation. Native side: KlGlfwNative (libklglfw).
    // =================================================================================================================

    /** Event record written by libklglfw's nPollEvents; keep in sync with KLGLFW_EV_* / struct klglfw_event in klglfw.c. */
    static final int EV_SIZE = 48, EV_CAP = 256;
    static final int EV_KEY = 1, EV_CHAR = 2, EV_MOUSE_BUTTON = 3, EV_CURSOR_POS = 4, EV_SCROLL = 5,
        EV_FRAMEBUFFER_SIZE = 6, EV_WINDOW_SIZE = 7, EV_FOCUS = 8, EV_CURSOR_ENTER = 9, EV_CLOSE = 10,
        EV_ICONIFY = 11, EV_CONTENT_SCALE = 12, EV_REFRESH = 13, EV_WINDOW_POS = 14;

    /** Per-window state. Callbacks are the caller's objects, invoked directly from glfwPollEvents. */
    static final class Win {
        final long handle;
        int width, height, fbWidth, fbHeight, x, y;
        CharSequence title;
        boolean shouldClose;
        long monitor;
        double cursorX, cursorY;
        final byte[] keys = new byte[GLFW_KEY_LAST + 1];
        final byte[] buttons = new byte[GLFW_MOUSE_BUTTON_LAST + 1];
        final Map<Integer, Integer> inputModes = new HashMap<>();
        final Map<Integer, Integer> attribs = new HashMap<>();
        GLFWKeyCallbackI key; GLFWCharCallbackI chr; GLFWCharModsCallbackI charMods;
        GLFWMouseButtonCallbackI mouseButton; GLFWCursorPosCallbackI cursorPos; GLFWCursorEnterCallbackI cursorEnter;
        GLFWScrollCallbackI scroll; GLFWDropCallbackI drop;
        GLFWFramebufferSizeCallbackI fbSize; GLFWWindowSizeCallbackI winSize; GLFWWindowPosCallbackI winPos;
        GLFWWindowCloseCallbackI close; GLFWWindowRefreshCallbackI refresh; GLFWWindowFocusCallbackI focus;
        GLFWWindowIconifyCallbackI iconify; GLFWWindowMaximizeCallbackI maximize; GLFWWindowContentScaleCallbackI contentScale;
        // LWJGL 3.4.1 IME callbacks (GLFWPreedit/IMEStatus/PreeditCandidate types exist only in 3.4.x, hence Object);
        // stored and freed, never invoked: visionOS text entry arrives as plain CHAR events. Inert for 1.21.x.
        Object preedit, imeStatus, preeditCandidate;
        final int[] preeditRect = new int[4];
        Win(long h) { handle = h; }
    }

    private static final LinkedHashMap<Long, Win> windows = new LinkedHashMap<>();
    private static GLFWErrorCallbackI errorCallback;
    private static GLFWMonitorCallbackI monitorCallback;
    private static GLFWJoystickCallbackI joystickCallback;
    private static final Map<Integer, Integer> windowHints = new HashMap<>();
    private static boolean initialized, pumping;
    private static ByteBuffer ring;
    private static long ringAddr;
    private static String clipboard;
    private static int monitorWidth = 1920, monitorHeight = 1080;
    private static GLFWVidMode videoMode;
    private static GLFWGammaRamp gammaRamp;

    protected GLFW() { throw new UnsupportedOperationException(); }

    static Win win(long window) {
        Win w = windows.get(window);
        if (w == null) {
            if (window == 0L && !windows.isEmpty()) return windows.values().iterator().next();
            throw new IllegalArgumentException("klglfw: no such window: 0x" + Long.toHexString(window));
        }
        return w;
    }

    /** The previous callback is handed back only if it already is the LWJGL Callback type (so free() is legal). */
    @SuppressWarnings("unchecked")
    private static <T> T prev(Object old, Class<T> type) { return type.isInstance(old) ? (T) old : null; }

    private static void error(int code, String msg) {
        System.err.println("[klglfw] GLFW error 0x" + Integer.toHexString(code) + ": " + msg);
        GLFWErrorCallbackI cb = errorCallback;
        if (cb == null) return;
        try (MemoryStack s = MemoryStack.stackPush()) { cb.invoke(code, memAddress(s.UTF8(msg))); }
    }

    private static int sizeProp(String prop, int def) {
        String v = System.getProperty(prop);
        if (v == null) return def;
        try { return Integer.parseInt(v.trim()); } catch (NumberFormatException e) { return def; }
    }

    // ---- init / terminate --------------------------------------------------------------------------------------

    public static boolean glfwInit() {
        if (initialized) return true;
        KlGlfwNative.load();
        String gl = System.getProperty("org.lwjgl.opengl.libname");
        if (!KlGlfwNative.nInit(gl)) {
            System.err.println("[klglfw] init failed: " + KlGlfwNative.nLastError());
            return false;
        }
        long mon = KlGlfwNative.nGetMonitorSize();
        monitorWidth = sizeProp("glfwstub.monitorWidth", (int) (mon >>> 32));
        monitorHeight = sizeProp("glfwstub.monitorHeight", (int) mon);
        videoMode = new GLFWVidMode(ByteBuffer.allocateDirect(GLFWVidMode.SIZEOF).order(ByteOrder.nativeOrder()));
        memPutInt(videoMode.address() + GLFWVidMode.WIDTH, monitorWidth);
        memPutInt(videoMode.address() + GLFWVidMode.HEIGHT, monitorHeight);
        memPutInt(videoMode.address() + GLFWVidMode.REDBITS, 8);
        memPutInt(videoMode.address() + GLFWVidMode.GREENBITS, 8);
        memPutInt(videoMode.address() + GLFWVidMode.BLUEBITS, 8);
        memPutInt(videoMode.address() + GLFWVidMode.REFRESHRATE, 60);
        ring = ByteBuffer.allocateDirect(EV_SIZE * EV_CAP).order(ByteOrder.nativeOrder());
        ringAddr = memAddress(ring);
        initialized = true;
        System.out.println("[klglfw] GLFW stand-in up: " + KlGlfwNative.nDescribe());
        return true;
    }

    public static void glfwTerminate() {
        if (!initialized) return;
        for (Win w : new ArrayList<>(windows.values())) glfwDestroyWindow(w.handle);
        KlGlfwNative.nTerminate();
        initialized = false;
    }

    public static void glfwInitHint(int hint, int value) { }
    public static void glfwInitAllocator(@Nullable GLFWAllocator allocator) { }
    public static int glfwGetPlatform() { return GLFW_PLATFORM_COCOA; }  // keeps the traced macOS branches (icons etc.)
    public static boolean glfwPlatformSupported(int platform) { return platform == GLFW_PLATFORM_COCOA; }

    public static void glfwGetVersion(@Nullable IntBuffer major, @Nullable IntBuffer minor, @Nullable IntBuffer rev) {
        if (major != null) major.put(major.position(), GLFW_VERSION_MAJOR);
        if (minor != null) minor.put(minor.position(), GLFW_VERSION_MINOR);
        if (rev != null) rev.put(rev.position(), GLFW_VERSION_REVISION);
    }
    public static void glfwGetVersion(@Nullable int[] major, @Nullable int[] minor, @Nullable int[] rev) {
        if (major != null) major[0] = GLFW_VERSION_MAJOR;
        if (minor != null) minor[0] = GLFW_VERSION_MINOR;
        if (rev != null) rev[0] = GLFW_VERSION_REVISION;
    }
    public static String glfwGetVersionString() {
        return GLFW_VERSION_MAJOR + "." + GLFW_VERSION_MINOR + "." + GLFW_VERSION_REVISION + " Klepton klglfw (EGL pbuffer)";
    }
    public static int glfwGetError(@Nullable PointerBuffer description) {
        if (description != null) description.put(description.position(), 0L);
        return GLFW_NO_ERROR;
    }

    // ---- callbacks (kept in Java; no closures) -----------------------------------------------------------------

    public static GLFWErrorCallback glfwSetErrorCallback(@Nullable GLFWErrorCallbackI cbfun) {
        Object o = errorCallback; errorCallback = cbfun; return prev(o, GLFWErrorCallback.class);
    }
    public static GLFWMonitorCallback glfwSetMonitorCallback(@Nullable GLFWMonitorCallbackI cbfun) {
        Object o = monitorCallback; monitorCallback = cbfun; return prev(o, GLFWMonitorCallback.class);
    }
    public static GLFWJoystickCallback glfwSetJoystickCallback(@Nullable GLFWJoystickCallbackI cbfun) {
        Object o = joystickCallback; joystickCallback = cbfun; return prev(o, GLFWJoystickCallback.class);
    }
    public static GLFWKeyCallback glfwSetKeyCallback(long window, @Nullable GLFWKeyCallbackI cbfun) {
        Win w = win(window); Object o = w.key; w.key = cbfun; return prev(o, GLFWKeyCallback.class);
    }
    public static GLFWCharCallback glfwSetCharCallback(long window, @Nullable GLFWCharCallbackI cbfun) {
        Win w = win(window); Object o = w.chr; w.chr = cbfun; return prev(o, GLFWCharCallback.class);
    }
    public static GLFWCharModsCallback glfwSetCharModsCallback(long window, @Nullable GLFWCharModsCallbackI cbfun) {
        Win w = win(window); Object o = w.charMods; w.charMods = cbfun; return prev(o, GLFWCharModsCallback.class);
    }
    public static GLFWMouseButtonCallback glfwSetMouseButtonCallback(long window, @Nullable GLFWMouseButtonCallbackI cbfun) {
        Win w = win(window); Object o = w.mouseButton; w.mouseButton = cbfun; return prev(o, GLFWMouseButtonCallback.class);
    }
    public static GLFWCursorPosCallback glfwSetCursorPosCallback(long window, @Nullable GLFWCursorPosCallbackI cbfun) {
        Win w = win(window); Object o = w.cursorPos; w.cursorPos = cbfun; return prev(o, GLFWCursorPosCallback.class);
    }
    public static GLFWCursorEnterCallback glfwSetCursorEnterCallback(long window, @Nullable GLFWCursorEnterCallbackI cbfun) {
        Win w = win(window); Object o = w.cursorEnter; w.cursorEnter = cbfun; return prev(o, GLFWCursorEnterCallback.class);
    }
    public static GLFWScrollCallback glfwSetScrollCallback(long window, @Nullable GLFWScrollCallbackI cbfun) {
        Win w = win(window); Object o = w.scroll; w.scroll = cbfun; return prev(o, GLFWScrollCallback.class);
    }
    public static GLFWDropCallback glfwSetDropCallback(long window, @Nullable GLFWDropCallbackI cbfun) {
        Win w = win(window); Object o = w.drop; w.drop = cbfun; return prev(o, GLFWDropCallback.class);
    }
    public static GLFWFramebufferSizeCallback glfwSetFramebufferSizeCallback(long window, @Nullable GLFWFramebufferSizeCallbackI cbfun) {
        Win w = win(window); Object o = w.fbSize; w.fbSize = cbfun; return prev(o, GLFWFramebufferSizeCallback.class);
    }
    public static GLFWWindowSizeCallback glfwSetWindowSizeCallback(long window, @Nullable GLFWWindowSizeCallbackI cbfun) {
        Win w = win(window); Object o = w.winSize; w.winSize = cbfun; return prev(o, GLFWWindowSizeCallback.class);
    }
    public static GLFWWindowPosCallback glfwSetWindowPosCallback(long window, @Nullable GLFWWindowPosCallbackI cbfun) {
        Win w = win(window); Object o = w.winPos; w.winPos = cbfun; return prev(o, GLFWWindowPosCallback.class);
    }
    public static GLFWWindowCloseCallback glfwSetWindowCloseCallback(long window, @Nullable GLFWWindowCloseCallbackI cbfun) {
        Win w = win(window); Object o = w.close; w.close = cbfun; return prev(o, GLFWWindowCloseCallback.class);
    }
    public static GLFWWindowRefreshCallback glfwSetWindowRefreshCallback(long window, @Nullable GLFWWindowRefreshCallbackI cbfun) {
        Win w = win(window); Object o = w.refresh; w.refresh = cbfun; return prev(o, GLFWWindowRefreshCallback.class);
    }
    public static GLFWWindowFocusCallback glfwSetWindowFocusCallback(long window, @Nullable GLFWWindowFocusCallbackI cbfun) {
        Win w = win(window); Object o = w.focus; w.focus = cbfun; return prev(o, GLFWWindowFocusCallback.class);
    }
    public static GLFWWindowIconifyCallback glfwSetWindowIconifyCallback(long window, @Nullable GLFWWindowIconifyCallbackI cbfun) {
        Win w = win(window); Object o = w.iconify; w.iconify = cbfun; return prev(o, GLFWWindowIconifyCallback.class);
    }
    public static GLFWWindowMaximizeCallback glfwSetWindowMaximizeCallback(long window, @Nullable GLFWWindowMaximizeCallbackI cbfun) {
        Win w = win(window); Object o = w.maximize; w.maximize = cbfun; return prev(o, GLFWWindowMaximizeCallback.class);
    }
    public static GLFWWindowContentScaleCallback glfwSetWindowContentScaleCallback(long window, @Nullable GLFWWindowContentScaleCallbackI cbfun) {
        Win w = win(window); Object o = w.contentScale; w.contentScale = cbfun; return prev(o, GLFWWindowContentScaleCallback.class);
    }

    /** Callbacks.glfwFreeCallbacks: drop every per-window callback; free() the ones that are LWJGL Callback objects. */
    static void freeWindowCallbacks(long window) {
        Win w = windows.get(window);
        if (w == null) return;
        Object[] all = { w.key, w.chr, w.charMods, w.mouseButton, w.cursorPos, w.cursorEnter, w.scroll, w.drop, w.fbSize,
            w.winSize, w.winPos, w.close, w.refresh, w.focus, w.iconify, w.maximize, w.contentScale,
            w.preedit, w.imeStatus, w.preeditCandidate };
        w.preedit = null; w.imeStatus = null; w.preeditCandidate = null;
        w.key = null; w.chr = null; w.charMods = null; w.mouseButton = null; w.cursorPos = null; w.cursorEnter = null;
        w.scroll = null; w.drop = null; w.fbSize = null; w.winSize = null; w.winPos = null; w.close = null;
        w.refresh = null; w.focus = null; w.iconify = null; w.maximize = null; w.contentScale = null;
        for (Object o : all) if (o instanceof Callback) ((Callback) o).free();
    }

    // ---- events ------------------------------------------------------------------------------------------------

    public static void glfwPollEvents() {
        if (!initialized || pumping) return;  // Minecraft re-enters glfwPollEvents from inside input handlers
        pumping = true;
        try {
            int n;
            do {
                n = KlGlfwNative.nPollEvents(ringAddr, EV_CAP);
                for (int i = 0; i < n; i++) dispatch(ringAddr + (long) i * EV_SIZE);
            } while (n == EV_CAP);
        } finally {
            pumping = false;
        }
    }

    private static void dispatch(long e) {
        long h = memGetLong(e);
        Win w = windows.get(h);
        if (w == null) { if (windows.isEmpty()) return; w = windows.values().iterator().next(); h = w.handle; }
        int type = memGetInt(e + 8), a = memGetInt(e + 12), b = memGetInt(e + 16), c = memGetInt(e + 20), d = memGetInt(e + 24);
        double x = memGetDouble(e + 32), y = memGetDouble(e + 40);
        switch (type) {
            case EV_KEY:
                if (a >= 0 && a <= GLFW_KEY_LAST) w.keys[a] = (byte) (c == GLFW_RELEASE ? GLFW_RELEASE : GLFW_PRESS);
                if (w.key != null) w.key.invoke(h, a, b, c, d);
                break;
            case EV_CHAR:
                if (w.chr != null) w.chr.invoke(h, a);
                if (w.charMods != null) w.charMods.invoke(h, a, d);
                break;
            case EV_MOUSE_BUTTON:
                if (a >= 0 && a <= GLFW_MOUSE_BUTTON_LAST) w.buttons[a] = (byte) b;
                if (w.mouseButton != null) w.mouseButton.invoke(h, a, b, c);
                break;
            case EV_CURSOR_POS:
                w.cursorX = x; w.cursorY = y;
                if (w.cursorPos != null) w.cursorPos.invoke(h, x, y);
                break;
            case EV_SCROLL:
                if (w.scroll != null) w.scroll.invoke(h, x, y);
                break;
            case EV_FRAMEBUFFER_SIZE:
                w.fbWidth = a; w.fbHeight = b;
                if (w.fbSize != null) w.fbSize.invoke(h, a, b);
                break;
            case EV_WINDOW_SIZE:
                w.width = a; w.height = b;
                if (w.winSize != null) w.winSize.invoke(h, a, b);
                break;
            case EV_FOCUS:
                w.attribs.put(GLFW_FOCUSED, a);
                if (w.focus != null) w.focus.invoke(h, a != 0);
                break;
            case EV_CURSOR_ENTER:
                w.attribs.put(GLFW_HOVERED, a);
                if (w.cursorEnter != null) w.cursorEnter.invoke(h, a != 0);
                break;
            case EV_CLOSE:
                w.shouldClose = true;
                if (w.close != null) w.close.invoke(h);
                break;
            case EV_ICONIFY:
                w.attribs.put(GLFW_ICONIFIED, a);
                if (w.iconify != null) w.iconify.invoke(h, a != 0);
                break;
            case EV_CONTENT_SCALE:
                if (w.contentScale != null) w.contentScale.invoke(h, (float) x, (float) y);
                break;
            case EV_REFRESH:
                if (w.refresh != null) w.refresh.invoke(h);
                break;
            case EV_WINDOW_POS:
                w.x = a; w.y = b;
                if (w.winPos != null) w.winPos.invoke(h, a, b);
                break;
            default:
                System.err.println("[klglfw] unknown event type " + type);
        }
    }

    public static void glfwWaitEvents() { KlGlfwNative.nWaitEvents(-1.0); glfwPollEvents(); }
    public static void glfwWaitEventsTimeout(double timeout) { KlGlfwNative.nWaitEvents(timeout); glfwPollEvents(); }
    public static void glfwPostEmptyEvent() { KlGlfwNative.nPostEmptyEvent(); }

    // ---- windows / contexts ------------------------------------------------------------------------------------

    public static void glfwDefaultWindowHints() { windowHints.clear(); }
    public static void glfwWindowHint(int hint, int value) { windowHints.put(hint, value); }
    public static void glfwWindowHintString(int hint, @NativeType("const char *") ByteBuffer value) { }
    public static void glfwWindowHintString(int hint, @NativeType("const char *") CharSequence value) { }

    public static long glfwCreateWindow(int width, int height, CharSequence title, long monitor, long share) {
        if (!initialized) { error(GLFW_NOT_INITIALIZED, "glfwCreateWindow before glfwInit"); return NULL; }
        width = sizeProp("glfwstub.windowWidth", width);
        height = sizeProp("glfwstub.windowHeight", height);
        long h = KlGlfwNative.nCreateWindow(width, height, share,
            windowHints.getOrDefault(GLFW_CONTEXT_VERSION_MAJOR, 1), windowHints.getOrDefault(GLFW_CONTEXT_VERSION_MINOR, 0),
            windowHints.getOrDefault(GLFW_SAMPLES, 0));
        if (h == NULL) { error(GLFW_PLATFORM_ERROR, "klglfw: window/context creation failed: " + KlGlfwNative.nLastError()); return NULL; }
        Win w = new Win(h);
        long fb = KlGlfwNative.nGetFramebufferSize(h);
        w.width = w.fbWidth = (int) (fb >>> 32);
        w.height = w.fbHeight = (int) fb;
        w.title = title;
        w.monitor = monitor;
        w.attribs.put(GLFW_VISIBLE, windowHints.getOrDefault(GLFW_VISIBLE, 1));
        w.attribs.put(GLFW_FOCUSED, 1);
        w.attribs.put(GLFW_HOVERED, 1);
        w.attribs.put(GLFW_RESIZABLE, windowHints.getOrDefault(GLFW_RESIZABLE, 1));
        w.attribs.put(GLFW_DECORATED, windowHints.getOrDefault(GLFW_DECORATED, 1));
        w.attribs.put(GLFW_CLIENT_API, GLFW_OPENGL_API);
        w.attribs.put(GLFW_CONTEXT_VERSION_MAJOR, windowHints.getOrDefault(GLFW_CONTEXT_VERSION_MAJOR, 3));
        w.attribs.put(GLFW_CONTEXT_VERSION_MINOR, windowHints.getOrDefault(GLFW_CONTEXT_VERSION_MINOR, 2));
        w.attribs.put(GLFW_OPENGL_PROFILE, windowHints.getOrDefault(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE));
        w.inputModes.put(GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        w.cursorX = w.width / 2.0; w.cursorY = w.height / 2.0;
        windows.put(h, w);
        System.out.println("[klglfw] window 0x" + Long.toHexString(h) + " " + w.width + "x" + w.height + " \"" + title + "\"");
        return h;
    }

    public static void glfwDestroyWindow(long window) {
        if (windows.remove(window) == null) return;
        KlGlfwNative.nDestroyWindow(window);
    }

    public static void glfwMakeContextCurrent(long window) {
        if (!KlGlfwNative.nMakeContextCurrent(window)) error(GLFW_PLATFORM_ERROR, "klglfw: make current failed: " + KlGlfwNative.nLastError());
    }
    public static long glfwGetCurrentContext() { return KlGlfwNative.nGetCurrentContext(); }
    public static void glfwSwapBuffers(long window) { KlGlfwNative.nSwapBuffers(window); }
    public static void glfwSwapInterval(int interval) { KlGlfwNative.nSwapInterval(interval); }
    public static boolean glfwExtensionSupported(CharSequence extension) { return KlGlfwNative.nExtensionSupported(extension.toString()); }
    public static boolean glfwExtensionSupported(ByteBuffer extension) { return glfwExtensionSupported(memUTF8(memAddress(extension))); }
    public static long glfwGetProcAddress(CharSequence procname) { return KlGlfwNative.nGetProcAddress(procname.toString()); }
    public static long glfwGetProcAddress(ByteBuffer procname) { return glfwGetProcAddress(memUTF8(memAddress(procname))); }

    public static boolean glfwWindowShouldClose(long window) { return win(window).shouldClose; }
    public static void glfwSetWindowShouldClose(long window, boolean value) { win(window).shouldClose = value; }
    public static void glfwSetWindowTitle(long window, @NativeType("char const *") ByteBuffer title) { win(window).title = memUTF8(memAddress(title)); }
    public static void glfwSetWindowTitle(long window, @NativeType("char const *") CharSequence title) { win(window).title = title; }
    public static void glfwSetWindowIcon(long window, @Nullable GLFWImage.Buffer images) { }
    public static void glfwSetWindowSizeLimits(long window, int minwidth, int minheight, int maxwidth, int maxheight) { }
    public static void glfwSetWindowAspectRatio(long window, int numer, int denom) { }
    public static void glfwSetWindowPos(long window, int xpos, int ypos) { Win w = win(window); w.x = xpos; w.y = ypos; }
    /** The pbuffer is fixed at creation; a resize request is recorded and logged only (T8: kl_egl surface owns size). */
    public static void glfwSetWindowSize(long window, int width, int height) {
        System.out.println("[klglfw] glfwSetWindowSize(" + width + "x" + height + ") ignored: surface is fixed at " + win(window).fbWidth + "x" + win(window).fbHeight);
    }
    public static void glfwShowWindow(long window) { win(window).attribs.put(GLFW_VISIBLE, 1); }
    public static void glfwHideWindow(long window) { win(window).attribs.put(GLFW_VISIBLE, 0); }
    public static void glfwFocusWindow(long window) { }
    public static void glfwIconifyWindow(long window) { }
    public static void glfwRestoreWindow(long window) { }
    public static void glfwMaximizeWindow(long window) { }
    public static void glfwRequestWindowAttention(long window) { }
    public static float glfwGetWindowOpacity(long window) { return 1f; }
    public static void glfwSetWindowOpacity(long window, float opacity) { }
    public static long glfwGetWindowUserPointer(long window) { return NULL; }
    public static void glfwSetWindowUserPointer(long window, long pointer) { }

    public static int glfwGetWindowAttrib(long window, int attrib) { return win(window).attribs.getOrDefault(attrib, 0); }
    public static void glfwSetWindowAttrib(long window, int attrib, int value) { win(window).attribs.put(attrib, value); }

    public static void glfwGetWindowPos(long window, @Nullable IntBuffer xpos, @Nullable IntBuffer ypos) {
        Win w = win(window);
        if (xpos != null) xpos.put(xpos.position(), w.x);
        if (ypos != null) ypos.put(ypos.position(), w.y);
    }
    public static void glfwGetWindowPos(long window, @Nullable int[] xpos, @Nullable int[] ypos) {
        Win w = win(window);
        if (xpos != null) xpos[0] = w.x;
        if (ypos != null) ypos[0] = w.y;
    }
    public static void glfwGetWindowSize(long window, @Nullable IntBuffer width, @Nullable IntBuffer height) {
        Win w = win(window);
        if (width != null) width.put(width.position(), w.width);
        if (height != null) height.put(height.position(), w.height);
    }
    public static void glfwGetWindowSize(long window, @Nullable int[] width, @Nullable int[] height) {
        Win w = win(window);
        if (width != null) width[0] = w.width;
        if (height != null) height[0] = w.height;
    }
    public static void glfwGetFramebufferSize(long window, @Nullable IntBuffer width, @Nullable IntBuffer height) {
        Win w = win(window);
        if (width != null) width.put(width.position(), w.fbWidth);
        if (height != null) height.put(height.position(), w.fbHeight);
    }
    public static void glfwGetFramebufferSize(long window, @Nullable int[] width, @Nullable int[] height) {
        Win w = win(window);
        if (width != null) width[0] = w.fbWidth;
        if (height != null) height[0] = w.fbHeight;
    }
    public static void glfwGetWindowFrameSize(long window, @Nullable IntBuffer left, @Nullable IntBuffer top, @Nullable IntBuffer right, @Nullable IntBuffer bottom) {
        if (left != null) left.put(left.position(), 0);
        if (top != null) top.put(top.position(), 0);
        if (right != null) right.put(right.position(), 0);
        if (bottom != null) bottom.put(bottom.position(), 0);
    }
    public static void glfwGetWindowFrameSize(long window, @Nullable int[] left, @Nullable int[] top, @Nullable int[] right, @Nullable int[] bottom) {
        if (left != null) left[0] = 0;
        if (top != null) top[0] = 0;
        if (right != null) right[0] = 0;
        if (bottom != null) bottom[0] = 0;
    }
    public static void glfwGetWindowContentScale(long window, @Nullable FloatBuffer xscale, @Nullable FloatBuffer yscale) {
        if (xscale != null) xscale.put(xscale.position(), 1f);
        if (yscale != null) yscale.put(yscale.position(), 1f);
    }
    public static void glfwGetWindowContentScale(long window, @Nullable float[] xscale, @Nullable float[] yscale) {
        if (xscale != null) xscale[0] = 1f;
        if (yscale != null) yscale[0] = 1f;
    }

    // ---- monitors ------------------------------------------------------------------------------------------------

    public static final long KLGLFW_MONITOR = 1L;

    @Nullable
    public static PointerBuffer glfwGetMonitors() {
        PointerBuffer p = PointerBuffer.allocateDirect(1);
        p.put(0, KLGLFW_MONITOR);
        return p;
    }
    public static long glfwGetPrimaryMonitor() { return KLGLFW_MONITOR; }
    public static void glfwGetMonitorPos(long monitor, @Nullable IntBuffer xpos, @Nullable IntBuffer ypos) {
        if (xpos != null) xpos.put(xpos.position(), 0);
        if (ypos != null) ypos.put(ypos.position(), 0);
    }
    public static void glfwGetMonitorPos(long monitor, @Nullable int[] xpos, @Nullable int[] ypos) {
        if (xpos != null) xpos[0] = 0;
        if (ypos != null) ypos[0] = 0;
    }
    public static void glfwGetMonitorWorkarea(long monitor, @Nullable IntBuffer xpos, @Nullable IntBuffer ypos, @Nullable IntBuffer width, @Nullable IntBuffer height) {
        if (xpos != null) xpos.put(xpos.position(), 0);
        if (ypos != null) ypos.put(ypos.position(), 0);
        if (width != null) width.put(width.position(), monitorWidth);
        if (height != null) height.put(height.position(), monitorHeight);
    }
    public static void glfwGetMonitorWorkarea(long monitor, @Nullable int[] xpos, @Nullable int[] ypos, @Nullable int[] width, @Nullable int[] height) {
        if (xpos != null) xpos[0] = 0;
        if (ypos != null) ypos[0] = 0;
        if (width != null) width[0] = monitorWidth;
        if (height != null) height[0] = monitorHeight;
    }
    public static void glfwGetMonitorPhysicalSize(long monitor, @Nullable IntBuffer widthMM, @Nullable IntBuffer heightMM) {
        if (widthMM != null) widthMM.put(widthMM.position(), monitorWidth * 254 / 960);   // 96 dpi
        if (heightMM != null) heightMM.put(heightMM.position(), monitorHeight * 254 / 960);
    }
    public static void glfwGetMonitorPhysicalSize(long monitor, @Nullable int[] widthMM, @Nullable int[] heightMM) {
        if (widthMM != null) widthMM[0] = monitorWidth * 254 / 960;
        if (heightMM != null) heightMM[0] = monitorHeight * 254 / 960;
    }
    public static void glfwGetMonitorContentScale(long monitor, @Nullable FloatBuffer xscale, @Nullable FloatBuffer yscale) {
        if (xscale != null) xscale.put(xscale.position(), 1f);
        if (yscale != null) yscale.put(yscale.position(), 1f);
    }
    public static void glfwGetMonitorContentScale(long monitor, @Nullable float[] xscale, @Nullable float[] yscale) {
        if (xscale != null) xscale[0] = 1f;
        if (yscale != null) yscale[0] = 1f;
    }
    @Nullable public static String glfwGetMonitorName(long monitor) { return "Klepton"; }
    public static long glfwGetMonitorUserPointer(long monitor) { return NULL; }
    public static void glfwSetMonitorUserPointer(long monitor, long pointer) { }
    @Nullable public static GLFWVidMode.Buffer glfwGetVideoModes(long monitor) { return GLFWVidMode.create(videoMode.address(), 1); }
    @Nullable public static GLFWVidMode glfwGetVideoMode(long monitor) { return videoMode; }
    public static void glfwSetGamma(long monitor, float gamma) { }
    @Nullable public static GLFWGammaRamp glfwGetGammaRamp(long monitor) { return gammaRamp; }
    public static void glfwSetGammaRamp(long monitor, GLFWGammaRamp ramp) { gammaRamp = ramp; }
    public static long glfwGetWindowMonitor(long window) { return win(window).monitor; }
    /** Fullscreen toggle: recorded only (the surface does not change size). */
    public static void glfwSetWindowMonitor(long window, long monitor, int xpos, int ypos, int width, int height, int refreshRate) {
        Win w = win(window); w.monitor = monitor; w.x = xpos; w.y = ypos;
    }

    // ---- LWJGL 3.4.1 only (Minecraft 26.x) ---------------------------------------------------------------------
    // The block below is a comment for the 1.21.x build (scripts/t4/build-t4.sh compiles this file against the
    // QuestCraft 3.3.3 fork, whose GLFW has no IME types). scripts/t2/build-klglfw341.sh strips the two marker lines
    // and compiles it against stock LWJGL 3.4.1. Keep block comments OUT of the block (they would end it early).
    /*#LWJGL341

    // LWJGL 3.4.1 GLFW constants the 3.3.3-based constants block above lacks (javap -constants of the stock jar).
    public static final int
        GLFW_ANGLE_PLATFORM_TYPE = 327682,
        GLFW_ANGLE_PLATFORM_TYPE_D3D11 = 225285,
        GLFW_ANGLE_PLATFORM_TYPE_D3D9 = 225284,
        GLFW_ANGLE_PLATFORM_TYPE_METAL = 225288,
        GLFW_ANGLE_PLATFORM_TYPE_NONE = 225281,
        GLFW_ANGLE_PLATFORM_TYPE_OPENGL = 225282,
        GLFW_ANGLE_PLATFORM_TYPE_OPENGLES = 225283,
        GLFW_ANGLE_PLATFORM_TYPE_VULKAN = 225287,
        GLFW_ANY_POSITION = 0x80000000,
        GLFW_CONTEXT_DEBUG = 139271,
        GLFW_CURSOR_CAPTURED = 212996,
        GLFW_IME = 208903,
        GLFW_MANAGE_PREEDIT_CANDIDATE = 327684,
        GLFW_MOUSE_PASSTHROUGH = 131085,
        GLFW_PLATFORM = 327683,
        GLFW_PLATFORM_UNAVAILABLE = 65550,
        GLFW_POSITION_X = 131086,
        GLFW_POSITION_Y = 131087,
        GLFW_SCALE_FRAMEBUFFER = 139277,
        GLFW_SOFT_FULLSCREEN = 131088,
        GLFW_UNLIMITED_MOUSE_BUTTONS = 208902,
        GLFW_WAYLAND_APP_ID = 155649,
        GLFW_WAYLAND_DISABLE_LIBDECOR = 229378,
        GLFW_WAYLAND_LIBDECOR = 339969,
        GLFW_WAYLAND_PREFER_LIBDECOR = 229377,
        GLFW_WIN32_SHOWDEFAULT = 151554,
        GLFW_X11_ONTHESPOT = 335874,
        GLFW_X11_XCB_VULKAN_SURFACE = 335873;

    private static SharedLibrary library;
    // Minecraft 26.2 NativeLibrariesBootstrap.loadGlfw() requires GLFW.getLibrary() != null (fatal crash report
    // otherwise). The "GLFW library" is libklglfw: same lookup as KlGlfwNative.load() (-Dklglfw.libpath, else
    // libklglfw.dylib on LWJGL's library path), opened as a SharedLibrary through LWJGL's own loader.
    public static synchronized SharedLibrary getLibrary() {
        if (library == null) {
            KlGlfwNative.load();
            String p = System.getProperty("klglfw.libpath");
            library = Library.loadNative(GLFW.class, "org.lwjgl.glfw", p != null ? p : Platform.mapLibraryNameBundled("klglfw"));
        }
        return library;
    }

    static void klError(int code, String msg) { error(code, msg); }

    // IME (LWJGL's GLFW carries the glfw IME patch set). Callbacks are stored like every other callback but never
    // fire; the preedit rectangle is recorded; candidates/preedit text are always empty.
    public static @Nullable GLFWPreeditCallback glfwSetPreeditCallback(long window, @Nullable GLFWPreeditCallbackI cbfun) {
        Win w = win(window); Object o = w.preedit; w.preedit = cbfun; return prev(o, GLFWPreeditCallback.class);
    }
    public static @Nullable GLFWIMEStatusCallback glfwSetIMEStatusCallback(long window, @Nullable GLFWIMEStatusCallbackI cbfun) {
        Win w = win(window); Object o = w.imeStatus; w.imeStatus = cbfun; return prev(o, GLFWIMEStatusCallback.class);
    }
    public static @Nullable GLFWPreeditCandidateCallback glfwSetPreeditCandidateCallback(long window, @Nullable GLFWPreeditCandidateCallbackI cbfun) {
        Win w = win(window); Object o = w.preeditCandidate; w.preeditCandidate = cbfun; return prev(o, GLFWPreeditCandidateCallback.class);
    }
    public static void glfwSetPreeditCursorRectangle(long window, int x, int y, int w, int h) {
        int[] r = win(window).preeditRect; r[0] = x; r[1] = y; r[2] = w; r[3] = h;
    }
    public static void glfwGetPreeditCursorRectangle(long window, @Nullable IntBuffer x, @Nullable IntBuffer y, @Nullable IntBuffer w, @Nullable IntBuffer h) {
        int[] r = win(window).preeditRect;
        if (x != null) x.put(x.position(), r[0]);
        if (y != null) y.put(y.position(), r[1]);
        if (w != null) w.put(w.position(), r[2]);
        if (h != null) h.put(h.position(), r[3]);
    }
    public static void glfwGetPreeditCursorRectangle(long window, @Nullable int[] x, @Nullable int[] y, @Nullable int[] w, @Nullable int[] h) {
        int[] r = win(window).preeditRect;
        if (x != null) x[0] = r[0];
        if (y != null) y[0] = r[1];
        if (w != null) w[0] = r[2];
        if (h != null) h[0] = r[3];
    }
    public static void glfwResetPreeditText(long window) { }
    public static @Nullable IntBuffer glfwGetPreeditCandidate(long window, int index) { return null; }

    #LWJGL341*/

    // ---- input ---------------------------------------------------------------------------------------------------

    public static int glfwGetInputMode(long window, int mode) { return win(window).inputModes.getOrDefault(mode, mode == GLFW_CURSOR ? GLFW_CURSOR_NORMAL : 0); }
    public static void glfwSetInputMode(long window, int mode, int value) {
        win(window).inputModes.put(mode, value);
        KlGlfwNative.nSetInputMode(window, mode, value);
    }
    public static boolean glfwRawMouseMotionSupported() { return false; }

    public static String glfwGetKeyName(int key, int scancode) {
        if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) return String.valueOf((char) (key + 32));
        if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) return String.valueOf((char) key);
        if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) return String.valueOf((char) ('0' + key - GLFW_KEY_KP_0));
        switch (key) {
            case GLFW_KEY_APOSTROPHE: return "'";
            case GLFW_KEY_COMMA: return ",";
            case GLFW_KEY_MINUS: return "-";
            case GLFW_KEY_PERIOD: return ".";
            case GLFW_KEY_SLASH: return "/";
            case GLFW_KEY_SEMICOLON: return ";";
            case GLFW_KEY_EQUAL: return "=";
            case GLFW_KEY_LEFT_BRACKET: return "[";
            case GLFW_KEY_BACKSLASH: return "\\";
            case GLFW_KEY_RIGHT_BRACKET: return "]";
            case GLFW_KEY_GRAVE_ACCENT: return "`";
            case GLFW_KEY_KP_DECIMAL: return ".";
            case GLFW_KEY_KP_DIVIDE: return "/";
            case GLFW_KEY_KP_MULTIPLY: return "*";
            case GLFW_KEY_KP_SUBTRACT: return "-";
            case GLFW_KEY_KP_ADD: return "+";
            case GLFW_KEY_KP_EQUAL: return "=";
            default: return null;
        }
    }
    public static int glfwGetKeyScancode(int key) { return key; }
    public static int glfwGetKey(long window, int key) {
        Win w = win(window);
        return key >= 0 && key <= GLFW_KEY_LAST ? w.keys[key] : GLFW_RELEASE;
    }
    public static int glfwGetMouseButton(long window, int button) {
        Win w = win(window);
        return button >= 0 && button <= GLFW_MOUSE_BUTTON_LAST ? w.buttons[button] : GLFW_RELEASE;
    }
    public static void glfwGetCursorPos(long window, @Nullable DoubleBuffer xpos, @Nullable DoubleBuffer ypos) {
        Win w = win(window);
        if (xpos != null) xpos.put(xpos.position(), w.cursorX);
        if (ypos != null) ypos.put(ypos.position(), w.cursorY);
    }
    public static void glfwGetCursorPos(long window, @Nullable double[] xpos, @Nullable double[] ypos) {
        Win w = win(window);
        if (xpos != null) xpos[0] = w.cursorX;
        if (ypos != null) ypos[0] = w.cursorY;
    }
    public static void glfwSetCursorPos(long window, double xpos, double ypos) {
        Win w = win(window); w.cursorX = xpos; w.cursorY = ypos;
        KlGlfwNative.nSetCursorPos(window, xpos, ypos);
    }
    public static long glfwCreateCursor(GLFWImage image, int xhot, int yhot) { return 4L; }
    public static long glfwCreateStandardCursor(int shape) { return 4L; }
    public static void glfwDestroyCursor(long cursor) { }
    public static void glfwSetCursor(long window, long cursor) { }

    public static boolean glfwJoystickPresent(int jid) { return false; }
    @Nullable public static FloatBuffer glfwGetJoystickAxes(int jid) { return null; }
    @Nullable public static ByteBuffer glfwGetJoystickButtons(int jid) { return null; }
    @Nullable public static ByteBuffer glfwGetJoystickHats(int jid) { return null; }
    @Nullable public static String glfwGetJoystickName(int jid) { return null; }
    @Nullable public static String glfwGetJoystickGUID(int jid) { return null; }
    public static long glfwGetJoystickUserPointer(int jid) { return NULL; }
    public static void glfwSetJoystickUserPointer(int jid, long pointer) { }
    public static boolean glfwJoystickIsGamepad(int jid) { return false; }
    public static boolean glfwUpdateGamepadMappings(ByteBuffer string) { return false; }
    @Nullable public static String glfwGetGamepadName(int jid) { return null; }
    public static boolean glfwGetGamepadState(int jid, GLFWGamepadState state) { return false; }

    /** Clipboard: process-local only (no pasteboard on this path; T8 may wire UIPasteboard). */
    public static void glfwSetClipboardString(long window, @NativeType("char const *") ByteBuffer string) { clipboard = memUTF8(memAddress(string)); }
    public static void glfwSetClipboardString(long window, @NativeType("char const *") CharSequence string) { clipboard = string.toString(); }
    @Nullable public static String glfwGetClipboardString(long window) { return clipboard; }

    // ---- time ------------------------------------------------------------------------------------------------------

    public static double glfwGetTime() { return KlGlfwNative.nGetTime(); }
    public static void glfwSetTime(double time) { KlGlfwNative.nSetTime(time); }
    public static long glfwGetTimerValue() { return KlGlfwNative.nGetTimerValue(); }
    public static long glfwGetTimerFrequency() { return 1_000_000_000L; }
}

import net.minecraft.world.phys.Vec3;
import org.joml.Matrix4d;
import org.joml.Matrix4f;
import org.joml.Matrix4fc;
import org.joml.Vector3d;
import org.joml.Vector3f;
import org.vivecraft.client_vr.gameplay.screenhandlers.HeadHudPitch;

import java.util.Arrays;

/**
 * Runnable check of HeadHudPitch with real JOML and Minecraft value classes.
 * The expected placement comes from an independent head-local oracle: the HUD offset is built in the head's
 * own frame as (0, r*sin(theta), -r*cos(theta)) and only then transformed to world space, instead of
 * re-running the helper's world-space Rodrigues rotation.
 */
public final class HudPitchTransformTest {
    private static int assertions;

    private static void check(boolean condition, String why) {
        assertions++;
        if (!condition) {
            throw new AssertionError(why);
        }
    }

    private static void near(double actual, double expected, double epsilon, String why) {
        check(Double.isFinite(actual) && Math.abs(actual - expected) <= epsilon,
            why + ": " + actual + " != " + expected + " (eps " + epsilon + ")");
    }

    private static float[] snapshot(Matrix4fc m) {
        return m.get(new float[16]);
    }

    private static void bitExact(Matrix4fc m, float[] snapshot, String why) {
        check(Arrays.equals(snapshot(m), snapshot), why);
    }

    private static Vector3d column(Matrix4fc m, double x, double y, double z) {
        return new Matrix4d(m).transformDirection(new Vector3d(x, y, z));
    }

    private static Vector3d delta(Vec3 to, Vec3 from) {
        return new Vector3d(to.x - from.x, to.y - from.y, to.z - from.z);
    }

    private static void nearVec(Vector3d actual, Vector3d expected, double epsilon, String why) {
        near(actual.x, expected.x, epsilon, why + " x");
        near(actual.y, expected.y, epsilon, why + " y");
        near(actual.z, expected.z, epsilon, why + " z");
    }

    private static double clampedRadians(float degrees) {
        return Math.toRadians(Math.max(-45.0, Math.min(45.0, degrees)));
    }

    /**
     * checks a pitched placement against the head-local oracle
     */
    private static void checkPlacement(
        HeadHudPitch.Placement result, Matrix4fc base, Vec3 head, Vec3 before, double radius, float degrees,
        String label)
    {
        double theta = clampedRadians(degrees);
        double c = Math.cos(theta);
        double s = Math.sin(theta);
        double tolerance = 1.0e-6 * (1.0 + radius);

        check(result != null, label + ": non-zero pitch produces a placement");

        // position: head-local oracle
        Vector3d wanted = new Matrix4d(base).transformDirection(new Vector3d(0.0, radius * s, -radius * c));
        Vector3d offset = delta(result.position(), head);
        nearVec(offset, wanted, tolerance, label + ": head-local position");

        // depth: radial distance to the head stays the same
        Vector3d original = delta(before, head);
        near(offset.length(), original.length(), 1.0e-8 * (1.0 + radius), label + ": radial HUD distance");

        // swept angle equals the clamped pitch
        double swept = Math.atan2(new Vector3d(original).cross(offset).length(), original.dot(offset));
        near(swept, Math.abs(theta), 1.0e-6, label + ": swept angle");

        // negative lowers along the head's own up axis, at any yaw/pitch/roll
        Vector3d baseRight = column(base, 1, 0, 0);
        Vector3d baseUp = column(base, 0, 1, 0);
        Vector3d baseBack = column(base, 0, 0, 1);
        double along = offset.dot(baseUp);
        near(along, radius * s, tolerance, label + ": head-local height");
        check(degrees < 0 ? along < 0 : along > 0, label + ": sign of head-local height");
        near(offset.dot(baseRight), 0.0, tolerance, label + ": no sideways drift");

        // orientation: rotated about its own right axis by the same angle
        Matrix4f rotation = result.rotation();
        nearVec(column(rotation, 1, 0, 0), baseRight, 1.0e-6, label + ": right axis unchanged");
        nearVec(column(rotation, 0, 1, 0),
            new Vector3d(baseUp).mul(c).add(new Vector3d(baseBack).mul(s)), 1.0e-6, label + ": pitched up axis");
        nearVec(column(rotation, 0, 0, 1),
            new Vector3d(baseUp).mul(-s).add(new Vector3d(baseBack).mul(c)), 1.0e-6, label + ": pitched back axis");

        // the quad still faces the head
        nearVec(column(rotation, 0, 0, -1), new Vector3d(offset).normalize(), 2.0e-6, label + ": quad faces head");

        // HUD size: the basis keeps its lengths, no scale introduced
        near(column(rotation, 1, 0, 0).length(), baseRight.length(), 1.0e-6, label + ": right length");
        near(column(rotation, 0, 1, 0).length(), baseUp.length(), 1.0e-6, label + ": up length");
        near(column(rotation, 0, 0, 1).length(), baseBack.length(), 1.0e-6, label + ": back length");
        near(rotation.m30(), base.m30(), 0.0, label + ": translation x untouched");
        near(rotation.m31(), base.m31(), 0.0, label + ": translation y untouched");
        near(rotation.m32(), base.m32(), 0.0, label + ": translation z untouched");

        // returned objects are new
        check(rotation != base, label + ": rotation is a new matrix");
        check(result.position() != before, label + ": position is a new vector");
    }

    private static void sweep() {
        Vec3 head = new Vec3(1000000.25, 65.75, -2000000.5);
        // radius = hudDistance * worldScale, covers small, default and huge world scales
        float[] radii = {0.025f, 0.5f, 2.0f, 6.0f, 40.0f};
        for (float yaw : new float[]{0, 90, 180, -90, 37, -143}) {
            for (float pitch : new float[]{0, 25, -35, 80}) {
                for (float roll : new float[]{0, 20, -25, 90}) {
                    for (float radius : radii) {
                        for (float degrees : new float[]{0, -15, 15, -45, 45, -2, 7.5f}) {
                            String label = "yaw " + yaw + " pitch " + pitch + " roll " + roll + " radius " +
                                radius + " deg " + degrees;
                            Matrix4f base = new Matrix4f()
                                .rotationY((float) Math.toRadians(yaw))
                                .rotateX((float) Math.toRadians(pitch))
                                .rotateZ((float) Math.toRadians(roll));
                            float[] baseSnapshot = snapshot(base);
                            Vector3d forward = new Matrix4d(base).transformDirection(new Vector3d(0, 0, -radius));
                            Vec3 before = new Vec3(head.x + forward.x, head.y + forward.y, head.z + forward.z);

                            HeadHudPitch.Placement result = HeadHudPitch.pitchInGameHud(false, false, head, before,
                                base, degrees);
                            bitExact(base, baseSnapshot, label + ": input rotation untouched");
                            check(before.x == head.x + forward.x && before.y == head.y + forward.y &&
                                before.z == head.z + forward.z, label + ": input position untouched");

                            if (degrees == 0) {
                                check(result == null, label + ": zero angle keeps original placement");
                            } else {
                                checkPlacement(result, base, head, before, radius, degrees, label);
                                if (pitch == 0 && roll == 0) {
                                    // upright head: head-local down is world down
                                    near(result.position().y - head.y, radius * Math.sin(clampedRadians(degrees)),
                                        1.0e-6 * (1.0 + radius), label + ": world height for upright head");
                                }
                            }

                            // screens (menus, inventory, forced to HUD) and the seated aim HUD keep their placement
                            check(HeadHudPitch.pitchInGameHud(true, false, head, before, base, -15) == null,
                                label + ": screen open excluded");
                            check(HeadHudPitch.pitchInGameHud(false, true, head, before, base, -15) == null,
                                label + ": seated alternate aim excluded");
                            check(HeadHudPitch.pitchInGameHud(true, true, head, before, base, -15) == null,
                                label + ": screen and seated alternate aim excluded");
                            bitExact(base, baseSnapshot, label + ": excluded input rotation untouched");
                        }
                    }
                }
            }
        }
    }

    /**
     * mirrors GuiHandler.extractGui's head branch: a fresh guirot is built from a shared eye pose,
     * the HUD position from the shared hmd direction. None of them may change.
     */
    private static void callsiteSharedPose() {
        for (float worldRotation : new float[]{0, 33, -120, 180}) {
            for (float worldScale : new float[]{0.1f, 1.0f, 3.7f, 20.0f}) {
                for (float hudDistance : new float[]{0.25f, 2.0f, 5.0f}) {
                    String label = "worldRotation " + worldRotation + " worldScale " + worldScale +
                        " hudDistance " + hudDistance;
                    // shared pose objects, like DH.vr.getEyeRotation(RenderPass.CENTER) and hmd.getDirection()
                    Matrix4f sharedEye = new Matrix4f().rotationYXZ(
                        (float) Math.toRadians(51), (float) Math.toRadians(-22), (float) Math.toRadians(13));
                    float[] sharedEyeSnapshot = snapshot(sharedEye);
                    float worldRadians = (float) Math.toRadians(worldRotation);
                    Vector3f sharedDirection = new Matrix4f().rotationY(worldRadians).mul(sharedEye)
                        .transformDirection(new Vector3f(0, 0, -1));
                    Vector3f directionSnapshot = new Vector3f(sharedDirection);

                    // the callsite's fresh matrix
                    Matrix4f guirot = new Matrix4f().rotationY(worldRadians);
                    guirot = guirot.mul(sharedEye, guirot);
                    float[] guirotSnapshot = snapshot(guirot);

                    Vec3 position = new Vec3(-1234.5, 70.25, 98765.125);
                    Vec3 guipos = new Vec3(
                        position.x + sharedDirection.x * worldScale * hudDistance,
                        position.y + sharedDirection.y * worldScale * hudDistance,
                        position.z + sharedDirection.z * worldScale * hudDistance);

                    HeadHudPitch.Placement result = HeadHudPitch.pitchInGameHud(false, false, position, guipos,
                        guirot, -15.0F);

                    bitExact(sharedEye, sharedEyeSnapshot, label + ": shared eye pose untouched");
                    check(sharedDirection.equals(directionSnapshot), label + ": shared direction untouched");
                    bitExact(guirot, guirotSnapshot, label + ": callsite guirot untouched");
                    check(result != null && result.rotation() != guirot && result.rotation() != sharedEye,
                        label + ": result owns a new matrix");

                    double radius = delta(guipos, position).length();
                    near(radius, worldScale * hudDistance, 1.0e-5 * (1.0 + radius), label + ": radius from worldScale");
                    checkPlacement(result, guirot, position, guipos, radius, -15.0F, label);
                }
            }
        }
    }

    private static void sign() {
        Vec3 head = Vec3.ZERO;
        Vec3 before = new Vec3(0, 0, -2);
        Matrix4f identity = new Matrix4f();

        HeadHudPitch.Placement down = HeadHudPitch.pitch(head, before, identity, -15.0F);
        near(Math.toDegrees(Math.atan2(down.position().y, -down.position().z)), -15.0, 1.0e-9,
            "negative 15 means downward 15 degrees");
        near(down.position().y, -2.0 * Math.sin(Math.toRadians(15)), 1.0e-12, "-15 height");
        near(down.position().z, -2.0 * Math.cos(Math.toRadians(15)), 1.0e-12, "-15 depth");
        near(down.position().x, 0.0, 1.0e-12, "-15 no sideways drift");
        // the quad tilts back to face the lowered head direction: its normal (+Z) gains +Y
        near(down.rotation().m21(), Math.sin(Math.toRadians(-15)) * -1.0, 1.0e-6, "-15 normal faces up to the eye");

        HeadHudPitch.Placement up = HeadHudPitch.pitch(head, before, identity, 15.0F);
        near(up.position().y, 2.0 * Math.sin(Math.toRadians(15)), 1.0e-12, "positive 15 means upward");

        // non origin head: rotation is about the head, not the world origin
        Vec3 offHead = new Vec3(10.5, -3.25, 7.0);
        HeadHudPitch.Placement moved = HeadHudPitch.pitch(offHead, offHead.add(0, 0, -2), identity, -15.0F);
        near(moved.position().x - offHead.x, down.position().x, 1.0e-12, "non origin head x");
        near(moved.position().y - offHead.y, down.position().y, 1.0e-12, "non origin head y");
        near(moved.position().z - offHead.z, down.position().z, 1.0e-12, "non origin head z");
    }

    private static void clamps() {
        Vec3 head = Vec3.ZERO;
        Vec3 before = new Vec3(0, 0, -2);
        Matrix4f identity = new Matrix4f();

        HeadHudPitch.Placement lower = HeadHudPitch.pitch(head, before, identity, -45.0F);
        HeadHudPitch.Placement upper = HeadHudPitch.pitch(head, before, identity, 45.0F);
        near(Math.toDegrees(Math.atan2(lower.position().y, -lower.position().z)), -45.0, 1.0e-9, "-45 exact");
        near(Math.toDegrees(Math.atan2(upper.position().y, -upper.position().z)), 45.0, 1.0e-9, "+45 exact");

        for (float beyond : new float[]{-45.0001F, -90.0F, -999.0F, -Float.MAX_VALUE}) {
            HeadHudPitch.Placement clamped = HeadHudPitch.pitch(head, before, identity, beyond);
            check(clamped.position().equals(lower.position()), beyond + " clamps to -45 position");
            check(Arrays.equals(snapshot(clamped.rotation()), snapshot(lower.rotation())),
                beyond + " clamps to -45 rotation");
        }
        for (float beyond : new float[]{45.0001F, 90.0F, 999.0F, Float.MAX_VALUE}) {
            HeadHudPitch.Placement clamped = HeadHudPitch.pitch(head, before, identity, beyond);
            check(clamped.position().equals(upper.position()), beyond + " clamps to +45 position");
            check(Arrays.equals(snapshot(clamped.rotation()), snapshot(upper.rotation())),
                beyond + " clamps to +45 rotation");
        }
        check(HeadHudPitch.MAX_PITCH_DEGREES == 45.0F, "clamp constant is 45 degrees");
    }

    private static void unchanged(Vec3 head, Vec3 before, Matrix4f rotation, float degrees, String why) {
        float[] rotationSnapshot = rotation == null ? null : snapshot(rotation);
        check(HeadHudPitch.pitch(head, before, rotation, degrees) == null, why + " keeps original placement");
        check(HeadHudPitch.pitchInGameHud(false, false, head, before, rotation, degrees) == null,
            why + " keeps original placement at the callsite entry");
        if (rotation != null) {
            bitExact(rotation, rotationSnapshot, why + " leaves rotation untouched");
        }
    }

    private static void invalid() {
        Vec3 head = Vec3.ZERO;
        Vec3 before = new Vec3(0, 0, -2);

        unchanged(head, before, new Matrix4f(), 0.0F, "zero angle");
        unchanged(head, before, new Matrix4f(), -0.0F, "negative zero angle");
        unchanged(head, before, new Matrix4f(), Float.NaN, "NaN angle");
        unchanged(head, before, new Matrix4f(), Float.POSITIVE_INFINITY, "+infinity angle");
        unchanged(head, before, new Matrix4f(), Float.NEGATIVE_INFINITY, "-infinity angle");

        unchanged(head, before, new Matrix4f().m00(Float.NaN), -15.0F, "NaN basis");
        unchanged(head, before, new Matrix4f().m11(Float.POSITIVE_INFINITY), -15.0F, "infinite basis");
        unchanged(head, before, new Matrix4f().m22(Float.NEGATIVE_INFINITY), -15.0F, "-infinite basis");
        unchanged(head, before, new Matrix4f().m00(0.0F), -15.0F, "zero right axis");
        unchanged(head, before, new Matrix4f().zero(), -15.0F, "zero basis");
        // right axis pointing at the HUD: pitching about it can't move the HUD consistently
        unchanged(head, new Vec3(2, 0, 0), new Matrix4f(), -15.0F, "HUD along the pitch axis");

        unchanged(head, head, new Matrix4f(), -15.0F, "HUD at the head");
        unchanged(new Vec3(Double.NaN, 0, 0), before, new Matrix4f(), -15.0F, "NaN head");
        unchanged(head, new Vec3(0, Double.POSITIVE_INFINITY, -2), new Matrix4f(), -15.0F, "infinite position");
        unchanged(new Vec3(-Double.MAX_VALUE, 0, 0), new Vec3(Double.MAX_VALUE, 0, 0), new Matrix4f(), -15.0F,
            "overflowing offset");
        unchanged(null, before, new Matrix4f(), -15.0F, "null head");
        unchanged(head, null, new Matrix4f(), -15.0F, "null position");
        unchanged(head, before, null, -15.0F, "null rotation");
    }

    public static void main(String[] args) {
        sweep();
        callsiteSharedPose();
        sign();
        clamps();
        invalid();
        System.out.println("PASS " + assertions +
            " HUD pitch angle/depth/orientation/exclusion/clamp/invalid-input assertions using real JOML and Vec3");
    }
}

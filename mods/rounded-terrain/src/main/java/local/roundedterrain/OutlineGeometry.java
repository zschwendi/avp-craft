package local.roundedterrain;

import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.blaze3d.vertex.VertexConsumer;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.concurrent.ConcurrentHashMap;
import local.roundedterrain.BevelMesh.Point;

/** Immutable, block-local selection lines from the same patches submitted for terrain. */
public final class OutlineGeometry {
    private static final double KEY_SCALE = 1_000_000d;
    private static final double MIN_LENGTH_SQUARED = 1e-12d;
    private static final double COPLANAR_DOT = 1d - 1e-5d;
    private static final int MAX_CACHED_MESHES = TerrainBevel.CACHE_LIMIT;
    // BevelMesh retains Object identity equality. Cache geometry contexts, never world positions.
    private static final ConcurrentHashMap<BevelMesh, List<Segment>> MESH_CACHE = new ConcurrentHashMap<>();

    public record Segment(Point start, Point end, Point direction) {}

    private record VertexKey(long x, long y, long z) implements Comparable<VertexKey> {
        static VertexKey of(Point p) {
            return new VertexKey(Math.round(p.x() * KEY_SCALE), Math.round(p.y() * KEY_SCALE),
                    Math.round(p.z() * KEY_SCALE));
        }

        @Override
        public int compareTo(VertexKey other) {
            int result = Long.compare(x, other.x);
            if (result == 0) result = Long.compare(y, other.y);
            if (result == 0) result = Long.compare(z, other.z);
            return result;
        }
    }

    private record EdgeKey(VertexKey start, VertexKey end) {
        static EdgeKey of(Point start, Point end) {
            VertexKey a = VertexKey.of(start), b = VertexKey.of(end);
            return a.compareTo(b) <= 0 ? new EdgeKey(a, b) : new EdgeKey(b, a);
        }
    }

    private static final class Edge {
        final Segment segment;
        final Point firstNormal;
        int uses = 1;
        boolean coplanar = true;

        Edge(Segment segment, Point normal) {
            this.segment = segment;
            this.firstNormal = normal;
        }

        void addFace(Point normal) {
            uses++;
            if (firstNormal == null || normal == null || dot(firstNormal, normal) < COPLANAR_DOT) {
                coplanar = false;
            }
        }
    }

    private OutlineGeometry() {}

    /**
     * Copies coordinates before deferred rendering. Coincident edges are emitted once; a shared edge between
     * two coplanar patches is omitted, so flat triangulation does not add a diagonal to the outline.
     */
    public static List<Segment> fromMesh(BevelMesh mesh) {
        if (mesh == null || mesh.patches.isEmpty()) return List.of();
        List<Segment> cached = MESH_CACHE.get(mesh);
        if (cached != null) return cached;
        synchronized (MESH_CACHE) {
            cached = MESH_CACHE.get(mesh);
            if (cached != null) return cached;
            List<Segment> result = fromPatches(mesh.patches);
            if (MESH_CACHE.size() < MAX_CACHED_MESHES) MESH_CACHE.put(mesh, result);
            return result;
        }
    }

    static List<Segment> fromPatches(List<BevelMesh.Patch> patches) {
        LinkedHashMap<EdgeKey, Edge> edges = new LinkedHashMap<>();
        for (BevelMesh.Patch patch : patches) {
            Point[] points = patch.points();
            if (points == null || points.length < 3) continue;
            Point normal = faceNormal(points);
            for (int i = 0; i < points.length; i++) {
                Point a = points[i], b = points[(i + 1) % points.length];
                if (!finite(a) || !finite(b)) continue;
                Point direction = unit(b.x() - a.x(), b.y() - a.y(), b.z() - a.z());
                if (direction == null) continue; // Spherical corner triangles use a repeated fourth point.
                EdgeKey key = EdgeKey.of(a, b);
                Edge previous = edges.get(key);
                if (previous == null) {
                    edges.put(key, new Edge(new Segment(copy(a), copy(b), direction), normal));
                } else {
                    previous.addFace(normal);
                }
            }
        }
        List<Segment> result = new ArrayList<>(edges.size());
        for (Edge edge : edges.values()) {
            if (edge.uses != 2 || !edge.coplanar) result.add(edge.segment);
        }
        return List.copyOf(result);
    }

    /** Same vertex attributes and segment-direction normals as MC26.3 ShapeOutlineFeatureRenderer. */
    public static void render(List<Segment> segments, PoseStack.Pose pose, VertexConsumer vertices,
                              int color, float width) {
        for (Segment segment : segments) {
            Point a = segment.start(), b = segment.end(), n = segment.direction();
            vertices.addVertex(pose, a.x(), a.y(), a.z()).setColor(color)
                    .setNormal(pose, n.x(), n.y(), n.z()).setLineWidth(width);
            vertices.addVertex(pose, b.x(), b.y(), b.z()).setColor(color)
                    .setNormal(pose, n.x(), n.y(), n.z()).setLineWidth(width);
        }
    }

    private static Point faceNormal(Point[] points) {
        if (!finite(points[0])) return null;
        Point origin = points[0];
        for (int a = 1; a < points.length - 1; a++) {
            if (!finite(points[a]) || !finite(points[a + 1])) continue;
            double ax = points[a].x() - origin.x(), ay = points[a].y() - origin.y(), az = points[a].z() - origin.z();
            double bx = points[a + 1].x() - origin.x(), by = points[a + 1].y() - origin.y(), bz = points[a + 1].z() - origin.z();
            Point normal = unit(ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx);
            if (normal != null) return normal;
        }
        return null;
    }

    private static Point unit(double x, double y, double z) {
        double squared = x * x + y * y + z * z;
        if (!Double.isFinite(squared) || squared <= MIN_LENGTH_SQUARED) return null;
        double inverse = 1d / Math.sqrt(squared);
        return new Point((float) (x * inverse), (float) (y * inverse), (float) (z * inverse));
    }

    private static double dot(Point a, Point b) {
        return (double) a.x() * b.x() + (double) a.y() * b.y() + (double) a.z() * b.z();
    }

    private static Point copy(Point p) { return new Point(p.x(), p.y(), p.z()); }

    private static boolean finite(Point p) {
        return p != null && Float.isFinite(p.x()) && Float.isFinite(p.y()) && Float.isFinite(p.z());
    }
}

package local.roundedterrain;

import java.util.List;

/** Selected-block geometry captured during extraction, consumed without reading the live world. */
public interface RoundedOutlineState {
    List<OutlineGeometry.Segment> roundedterrain$getOutline();
    void roundedterrain$setOutline(List<OutlineGeometry.Segment> segments);
}

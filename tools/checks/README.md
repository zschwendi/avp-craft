# Source regression fixtures

After `python3 tools/build.py --version VERSION` succeeds, run:

```sh
python3 tools/check_source.py
```

Pass `--jdk JDK_HOME` if Java 25 is not selected by `JAVA_HOME` or system detection. `--build BUILD_DIRECTORY` selects another build receipt and its three owned release jars. Compiler dependencies must already be present in the ignored build cache; this checker downloads nothing.

The runner verifies the current frozen source manifest, artifact hashes and sizes, and presentation header against the native build input. Java fixtures compile and execute against the packaged production classes, using the Minecraft dependencies only for the actual client ABI. Generated classes and the C executable are temporary. The result and exact source/artifact hashes are saved to ignored `build/source-checks/source-check-receipt.json`.

- `present_slots.c`: every three-slot occupancy/next-slot state, the false-full counterexample, 10,000 mailbox replacements and held-reader ownership.
- `MeshTopology.java`: quarter-circle and spherical geometry, continuous unit normals, winding and seams across exhaustive selected/unselected neighboring configurations, multiple radii and subdivisions.
- `OutlineGeometryCheck.java`: matching selection-line geometry, copied poses/coordinates, complete vertex attributes, duplicate/coplanar edge handling and bounded identity caching using the actual client interfaces.
- `NeutralMaterialCheck.java`: normal-map provenance and validity, neutral classification and marker, compact shared storage, all supported material IDs, exact sprite coverage, reload isolation and replicated real-map gutters.

These fixtures establish CPU behavior and compatibility with the built classes. They do not launch Minecraft, initialize a GPU, measure display pacing or establish visual acceptance.

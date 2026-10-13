/* Copyright 2026 mcopt-shade contributors. Licensed under Apache-2.0. */
package mcopt.metal.shade;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.List;

/** Standalone CPU fixtures: no game, GPU, pack assets, accounts, or native library initialization. */
public final class NeutralMaterialCheck {
    private static int assertions, fixtures;
    private static final int SIZE=128,PAD=2,CELL=132,TABLE=256;

    private static void check(boolean value, String message) {
        assertions++;
        if (!value) throw new AssertionError(message);
    }

    private static void equal(float actual, float expected, String message) {
        check(Math.abs(actual-expected)<1e-6f,message+": "+actual+" != "+expected);
    }

    private static float at(ByteBuffer bounds, int id, int row, int component) {
        return bounds.getFloat((row*TABLE+id)*16+component*4);
    }

    private static void rejected(Runnable action, String message) {
        try { action.run(); throw new AssertionError(message); }
        catch (IllegalArgumentException expected) { assertions++; }
    }

    private record Fixture(String label, String namespace, String path, String diffusePack, String normalPack,
                           int width, int height, boolean animated, boolean decoded,
                           boolean expectedSolid, boolean expectedReal, boolean expectedPom) {}

    private static void provenanceFixtures() {
        List<Fixture> cases=List.of(
            new Fixture("vanilla missing map","minecraft","block/stone","vanilla",null,0,0,false,false,true,false,false),
            new Fixture("replacement skin rejects inherited normal","minecraft","block/dirt","replacement","lower-pack",128,128,false,true,true,false,false),
            new Fixture("missing diffuse rejects orphan normal","minecraft","block/sand",null,"pack",128,128,false,true,true,false,false),
            new Fixture("corrupt map keeps terrain classification","minecraft","block/gravel","pack","pack",128,128,false,false,true,false,false),
            new Fixture("strip map keeps terrain classification","minecraft","block/grass_block_side","pack","pack",128,256,false,true,true,false,false),
            new Fixture("animated square map stays neutral","minecraft","block/grass_block_top","pack","pack",128,128,true,true,true,false,false),
            new Fixture("valid terrain sidecar","minecraft","block/cobblestone","pack","pack",128,128,false,true,true,true,true),
            new Fixture("grass overlay never POM","minecraft","block/grass_block_side_overlay","pack","pack",128,128,false,true,true,true,false),
            new Fixture("snowy grass never POM","minecraft","block/grass_block_snow","pack","pack",128,128,false,true,true,true,false),
            new Fixture("unmatched planks remain ID zero","minecraft","block/oak_planks","vanilla",null,0,0,false,false,false,false,false),
            new Fixture("modded same-name sprite is not vanilla terrain","mod","block/dirt","mod",null,0,0,false,false,false,false,false),
            new Fixture("nested dirt name is not canonical terrain","minecraft","block/custom/dirt","pack",null,0,0,false,false,false,false,false),
            new Fixture("real nonterrain map remains available","minecraft","block/oak_planks","pack","pack",128,128,false,true,false,true,false)
        );
        for (Fixture f:cases) {
            fixtures++;
            boolean solid=ShadeMaterials.Policy.solidSurface(f.namespace(),f.path());
            boolean real=ShadeMaterials.Policy.mapCandidate(false,f.diffusePack(),f.normalPack()) &&
                ShadeMaterials.Policy.staticMap(f.width(),f.height(),f.animated()) && f.decoded();
            boolean pom=real && ShadeMaterials.Policy.parallax(f.path());
            check(solid==f.expectedSolid(),f.label()+" solid");
            check(real==f.expectedReal(),f.label()+" map provenance and validity");
            check(pom==f.expectedPom(),f.label()+" POM");
            if (!real && !solid) continue;
            var layout=ShadeMaterials.Policy.layout(real ? 1 : 0,!real);
            ByteBuffer bounds=ByteBuffer.allocate(TABLE*3*16).order(ByteOrder.nativeOrder()),grid=ByteBuffer.allocate(4);
            ShadeMaterials.Policy.mapping(bounds,grid,1,32,32,0,0,16,16,0f,0f,.5f,.5f,
                layout,real ? 0 : -1,32,224,pom,solid);
            check((grid.get(0)&255)==1 && grid.get(1)==0 && grid.get(2)==0 && grid.get(3)==0,f.label()+" exact sprite coverage");
            equal(at(bounds,1,2,3),solid ? 1f : 0f,f.label()+" solid metadata");
            equal(at(bounds,1,2,2),real ? (pom ? 1f : 0f) : -1f,f.label()+" POM/neutral metadata");
            if (!real) {
                equal(at(bounds,1,2,0),1f,f.label()+" neutral minimum height");
                equal(at(bounds,1,2,1),1f,f.label()+" neutral maximum height");
                check(layout.width()==2 && layout.height()==2,f.label()+" 16-byte neutral atlas");
            }
        }
        check(!ShadeMaterials.Policy.matchingPack("PACK","pack"),"pack IDs are exact, case-sensitive provenance");
        check(!ShadeMaterials.Policy.matchingPack(null,null),"missing resources never match");
        fixtures++;
        check(!ShadeMaterials.Policy.mapCandidate(true,"pack","pack") && ShadeMaterials.Policy.solidSurface("minecraft","block/grass_block_top"),
            "animated diffuse remains neutral classified terrain, never a real static sidecar");
        check(ShadeMaterials.Policy.rank("minecraft","block/stone")<ShadeMaterials.Policy.rank("mod","block/dirt"),"known terrain IDs have priority over unrelated normal maps");
        for (int size:new int[]{1,16,128,4096}) check(ShadeMaterials.Policy.staticMap(size,size,false),"valid static map size");
        for (int size:new int[]{-1,0,4097}) check(!ShadeMaterials.Policy.staticMap(size,size,false),"reject invalid map size");
    }

    private static void layoutsAndPixels() {
        var neutral=ShadeMaterials.Policy.layout(0,true);
        ByteBuffer tiny=ByteBuffer.allocate(neutral.width()*neutral.height()*4);
        ShadeMaterials.Policy.neutralPixels(tiny);
        check(tiny.capacity()==16,"no real maps need exactly 16 pixel bytes");
        for (int o=0;o<tiny.capacity();o+=4) {
            check((tiny.get(o)&255)==128 && (tiny.get(o+1)&255)==128,"neutral tangent normal");
            check((tiny.get(o+2)&255)==255 && (tiny.get(o+3)&255)==255,"neutral AO and exposed height");
        }
        for (boolean shared:new boolean[]{false,true}) for (int n=1;n<=(shared ? 224 : 225);n++) {
            var layout=ShadeMaterials.Policy.layout(n,shared);
            int cells=n+(shared ? 1 : 0);
            check(layout.columns()<=15,"bounded atlas columns");
            check(layout.width()*layout.height()>=cells*CELL*CELL,"all decoded tiles fit");
            for (int cell=0;cell<cells;cell++) {
                int tx=layout.x(cell),ty=layout.y(cell);
                check(tx>=PAD && ty>=PAD && tx+SIZE+PAD<=layout.width() && ty+SIZE+PAD<=layout.height(),"content plus gutter fits atlas");
            }
        }
        rejected(()->ShadeMaterials.Policy.layout(0,false),"empty layout must not upload");
        rejected(()->ShadeMaterials.Policy.layout(-1,true),"negative tile count");
        rejected(()->ShadeMaterials.Policy.layout(226,false),"more than 225 IDs");
        rejected(()->ShadeMaterials.Policy.layout(225,true),"shared neutral requires its own material ID");

        var mixed=ShadeMaterials.Policy.layout(1,true);
        ByteBuffer pixels=ByteBuffer.allocate(mixed.width()*mixed.height()*4);
        ShadeMaterials.Policy.neutralPixels(pixels);
        byte[] content=new byte[SIZE*SIZE*4];
        for (int y=0;y<SIZE;y++) for (int x=0;x<SIZE;x++) {
            int o=(y*SIZE+x)*4;
            content[o]=(byte)x; content[o+1]=(byte)y; content[o+2]=(byte)201; content[o+3]=(byte)(x^y);
        }
        ShadeMaterials.Policy.tilePixels(pixels,mixed,1,content);
        int tx=mixed.x(1),ty=mixed.y(1);
        for (int y=-PAD;y<SIZE+PAD;y++) for (int x=-PAD;x<SIZE+PAD;x++) {
            int src=(Math.clamp(y,0,SIZE-1)*SIZE+Math.clamp(x,0,SIZE-1))*4,o=((ty+y)*mixed.width()+tx+x)*4;
            for (int c=0;c<4;c++) check(pixels.get(o+c)==content[src+c],"real normal tile preserves bytes and replicated gutters");
        }
        for (int y=0;y<CELL;y++) for (int x=0;x<CELL;x++) {
            int o=(y*mixed.width()+x)*4;
            check((pixels.get(o)&255)==128 && (pixels.get(o+1)&255)==128 && (pixels.get(o+2)&255)==255 && (pixels.get(o+3)&255)==255,"real tile never overwrites shared neutral cell");
        }
    }

    private static void exactCoverageAndReload() {
        ByteBuffer bounds=ByteBuffer.allocate(TABLE*3*16).order(ByteOrder.nativeOrder()),grid=ByteBuffer.allocate(225);
        var neutral=ShadeMaterials.Policy.layout(0,true);
        for (int id=1;id<=225;id++) {
            int x=(id-1)%15*16,y=(id-1)/15*16;
            ShadeMaterials.Policy.mapping(bounds,grid,id,240,240,x,y,x+16,y+16,x/240f,y/240f,(x+16)/240f,(y+16)/240f,
                neutral,-1,0,255,true,true);
            check((grid.get(id-1)&255)==id,"unsigned R8 ID survives for every supported material");
            equal(at(bounds,id,0,0),x/240f,"exact diffuse UV minimum");
            equal(at(bounds,id,0,3),(y+16)/240f,"exact diffuse UV maximum");
            equal(at(bounds,id,1,0),0f,"shared neutral UV minimum");
            equal(at(bounds,id,1,3),1f,"shared neutral UV maximum");
            equal(at(bounds,id,2,2),-1f,"neutral marker disables POM and normal-map work even if caller requested it");
            equal(at(bounds,id,2,3),1f,"neutral solid surface classification");
        }
        for (int row=0;row<3;row++) for (int c=0;c<4;c++) equal(at(bounds,0,row,c),0f,"ID zero stays unclassified");
        ByteBuffer reloadedBounds=ByteBuffer.allocate(TABLE*3*16).order(ByteOrder.nativeOrder()),reloadedGrid=ByteBuffer.allocate(4);
        ShadeMaterials.Policy.mapping(reloadedBounds,reloadedGrid,1,32,32,16,16,32,32,.5f,.5f,1f,1f,neutral,-1,255,255,false,true);
        check(reloadedGrid.get(0)==0 && reloadedGrid.get(1)==0 && reloadedGrid.get(2)==0 && reloadedGrid.get(3)==1,"reload maps only the new sprite location");
        equal(at(reloadedBounds,225,2,3),0f,"reload does not carry old IDs");
        for (int id:new int[]{0,226}) rejected(()->ShadeMaterials.Policy.mapping(bounds,grid,id,240,240,0,0,16,16,0f,0f,1/15f,1/15f,neutral,-1,255,255,false,true),"invalid ID must reject before writing");
        for (int[] r:new int[][]{{-16,0,16,16},{0,0,0,16},{0,0,15,16},{0,0,256,16},{0,-16,16,16},{0,0,16,256}})
            check(!ShadeMaterials.Policy.rectangle(r[0],r[1],r[2],r[3],240,240),"out-of-bounds/unaligned sprite must remain unmapped");
        check(!ShadeMaterials.Policy.rectangle(0,0,16,16,239,240),"unaligned atlas cannot map exact grid");

        var mixed=ShadeMaterials.Policy.layout(1,true);
        ByteBuffer mixedBounds=ByteBuffer.allocate(TABLE*3*16).order(ByteOrder.nativeOrder()),mixedGrid=ByteBuffer.allocate(4);
        ShadeMaterials.Policy.mapping(mixedBounds,mixedGrid,1,32,32,0,0,16,16,0f,0f,.5f,.5f,mixed,-1,0,255,true,true);
        ShadeMaterials.Policy.mapping(mixedBounds,mixedGrid,2,32,32,16,0,32,16,.5f,0f,1f,.5f,mixed,1,32,224,true,true);
        check(at(mixedBounds,1,1,2)<at(mixedBounds,2,1,0),"real and neutral sampling rectangles are separated by gutters");
        equal(at(mixedBounds,1,2,2),-1f,"neutral marker preserved in mixed atlas");
        equal(at(mixedBounds,2,2,0),32/255f,"real map minimum height retained");
        equal(at(mixedBounds,2,2,1),224/255f,"real map maximum height retained");
        equal(at(mixedBounds,2,2,2),1f,"real POM eligibility retained");
    }

    public static void main(String[] args) {
        provenanceFixtures(); layoutsAndPixels(); exactCoverageAndReload();
        System.out.println("{\"result\":\"PASS\",\"fixtures\":"+fixtures+",\"assertions\":"+assertions+",\"neutral_pixel_bytes\":16,\"material_id_limit\":225,\"scope\":\"CPU policy/layout; actual Minecraft 26.3 ABI compilation; no GPU/game launch\"}");
    }
}

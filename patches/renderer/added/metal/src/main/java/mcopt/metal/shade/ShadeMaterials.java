/* Copyright 2026 mcopt-shade contributors. Licensed under Apache-2.0. */
package mcopt.metal.shade;

import com.mojang.blaze3d.platform.NativeImage;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.GpuFormat;
import com.mojang.renderpearl.api.textures.GpuTexture;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import mcopt.metal.MetalBridge;
import net.minecraft.client.renderer.texture.SpriteLoader;
import net.minecraft.client.renderer.texture.TextureAtlasSprite;
import net.minecraft.resources.Identifier;
import net.minecraft.server.packs.resources.Resource;
import net.minecraft.server.packs.resources.ResourceManager;
import org.lwjgl.system.MemoryUtil;

/**
 * Static block LabPBR normals/AO/height and vanilla solid-terrain classification.
 * At most 225 sprite IDs map into a compact atlas; sprites without a valid matching
 * map share a neutral tile. Without real maps that atlas is only 2x2 pixels.
 * No full-size mirrored high-resolution block atlas is allocated. Render thread only; native resource slots
 * retain old generations until their in-flight argument-buffer ring has retired.
 */
public final class ShadeMaterials {
    public static final boolean ON = ShadeConfig.HDR && ShadeConfig.flag("mcopt.shade.materials", true);
    public static final float STRENGTH = ShadeConfig.floatFlag("mcopt.shade.materials.normal", 1f, 0f, 2f);
    public static final float DEPTH = ShadeConfig.floatFlag("mcopt.shade.materials.parallax", 0.20f, 0f, 0.25f);
    private static final int SIZE=128, PAD=2, CELL=SIZE+2*PAD, MAX=225, TABLE=256, GRID=16;
    private static Pending pending;
    private static GpuTexture normals, ids, table;
    private record Pending(SpriteLoader.Preparations atlas, ResourceManager resources) {}
    private record Material(byte[] pixels, int min, int max) {}
    private record Tile(TextureAtlasSprite sprite, Material material, int x0, int y0, int x1, int y1, boolean pom, boolean solidSurface) {}

    /** Pure policy and layout helpers also exercised by the standalone Java fixture. */
    static final class Policy {
        private static final List<String> PRIORITY=List.of("dirt", "coarse_dirt", "rooted_dirt", "grass_block_top", "grass_block_side",
            "grass_block_side_overlay", "sand", "red_sand", "gravel", "stone", "cobblestone", "mossy_cobblestone",
            "deepslate", "cobbled_deepslate", "andesite", "diorite", "granite", "stone_bricks", "mud", "packed_mud",
            "clay", "netherrack", "end_stone", "tuff", "calcite", "snow", "sandstone", "red_sandstone");

        static int rank(String namespace, String path) {
            if (!solidSurface(namespace,path)) return PRIORITY.size();
            String base=path.substring(path.lastIndexOf('/')+1);
            int index=PRIORITY.indexOf(base);
            if (base.equals("grass_block_snow")) index=PRIORITY.size()-1;
            return index<0 ? PRIORITY.size() : index;
        }

        static boolean solidSurface(String namespace, String path) {
            if (!namespace.equals("minecraft") || !path.startsWith("block/")) return false;
            String base=path.substring("block/".length());
            return PRIORITY.contains(base) || base.equals("grass_block_snow");
        }

        static boolean parallax(String path) {
            String base=path.substring(path.lastIndexOf('/')+1);
            return PRIORITY.contains(base) && !base.endsWith("overlay");
        }

        static boolean matchingPack(String diffusePack, String normalPack) {
            return diffusePack!=null && normalPack!=null && diffusePack.equals(normalPack);
        }

        static boolean mapCandidate(boolean diffuseAnimated, String diffusePack, String normalPack) {
            return !diffuseAnimated && matchingPack(diffusePack,normalPack);
        }

        static boolean staticMap(int width, int height, boolean animated) {
            return width>=1 && width<=4096 && width==height && !animated;
        }

        static boolean rectangle(int x0, int y0, int x1, int y1, int width, int height) {
            return width>0 && height>0 && width%GRID==0 && height%GRID==0 &&
                x0>=0 && y0>=0 && x1>x0 && y1>y0 && x1<=width && y1<=height && (x0|y0|x1|y1)%GRID==0;
        }

        record Layout(int width, int height, int columns, boolean neutral) {
            int x(int cell) { return (cell%columns)*CELL+PAD; }
            int y(int cell) { return (cell/columns)*CELL+PAD; }
        }

        static Layout layout(int realMaps, boolean neutral) {
            if (realMaps<0 || realMaps+(neutral ? 1 : 0)>MAX || (realMaps==0 && !neutral)) throw new IllegalArgumentException("invalid material count");
            if (realMaps==0) return new Layout(2,2,1,true);
            int cells=realMaps+(neutral ? 1 : 0), cols=Math.min(15,cells);
            return new Layout(cols*CELL,((cells+cols-1)/cols)*CELL,cols,neutral);
        }

        static void neutralPixels(ByteBuffer pixels) {
            for (int o=0;o<pixels.capacity();o+=4) pixels.put(o,(byte)128).put(o+1,(byte)128).put(o+2,(byte)255).put(o+3,(byte)255);
        }

        static void tilePixels(ByteBuffer pixels, Layout layout, int cell, byte[] content) {
            int tx=layout.x(cell),ty=layout.y(cell);
            for (int y=-PAD;y<SIZE+PAD;y++) for (int x=-PAD;x<SIZE+PAD;x++) {
                int src=(Math.clamp(y,0,SIZE-1)*SIZE+Math.clamp(x,0,SIZE-1))*4;
                int o=((ty+y)*layout.width()+tx+x)*4;
                for (int c=0;c<4;c++) pixels.put(o+c,content[src+c]);
            }
        }

        static void mapping(ByteBuffer bounds, ByteBuffer grid, int id, int aw, int ah,
                            int x0, int y0, int x1, int y1, float u0, float v0, float u1, float v1,
                            Layout layout, int cell, int min, int max, boolean pom, boolean solid) {
            if (id<1 || id>MAX || !rectangle(x0,y0,x1,y1,aw,ah)) throw new IllegalArgumentException("invalid sprite mapping");
            put(bounds,id,0,u0,v0,u1,v1);
            if (cell<0) {
                if (!layout.neutral()) throw new IllegalArgumentException("missing neutral tile");
                if (layout.width()==2) put(bounds,id,1,0f,0f,1f,1f);
                else put(bounds,id,1,(float)PAD/layout.width(),(float)PAD/layout.height(),(float)(PAD+SIZE)/layout.width(),(float)(PAD+SIZE)/layout.height());
                // Negative POM metadata identifies shared neutral data, so shaders can
                // skip tangent-basis and texture-sample work while retaining solidity.
                put(bounds,id,2,1f,1f,-1f,solid ? 1f : 0f);
            } else {
                int tx=layout.x(cell),ty=layout.y(cell);
                put(bounds,id,1,(float)tx/layout.width(),(float)ty/layout.height(),(float)(tx+SIZE)/layout.width(),(float)(ty+SIZE)/layout.height());
                put(bounds,id,2,min/255f,max/255f,pom ? 1f : 0f,solid ? 1f : 0f);
            }
            int gw=aw/GRID;
            for (int y=y0/GRID;y<y1/GRID;y++) for (int x=x0/GRID;x<x1/GRID;x++) grid.put(y*gw+x,(byte)id);
        }

        private static void put(ByteBuffer table,int id,int row,float x,float y,float z,float w) {
            int at=(row*TABLE+id)*16;
            table.putFloat(at,x).putFloat(at+4,y).putFloat(at+8,z).putFloat(at+12,w);
        }
    }

    private ShadeMaterials() {}

    /** Called after the block atlas has published its new sprite positions, including F3+T reloads. */
    public static void reload(SpriteLoader.Preparations atlas, ResourceManager resources) {
        if (ON) pending=new Pending(atlas,resources);
    }

    /** Called after ShadeBindings.nextSlot(), before any world draw. No steady-state resource scans or uploads. */
    public static void frame() {
        if (!ON || pending==null) return;
        Pending build=pending;
        pending=null;
        GpuTexture[] next=new GpuTexture[3];
        try {
            build(build,next);
            ShadeNative.resource(7,next[0]==null ? 0 : MetalBridge.textureHandle(next[0]));
            ShadeNative.resource(8,next[1]==null ? 0 : MetalBridge.textureHandle(next[1]));
            ShadeNative.resource(9,next[2]==null ? 0 : MetalBridge.textureHandle(next[2]));
            close(normals); close(ids); close(table);
            normals=next[0]; ids=next[1]; table=next[2];
        } catch (Exception failure) {
            for (GpuTexture texture:next) close(texture);
            // A failed or unmapped new pack must not sample a stale previous pack's positions.
            ShadeNative.resource(7,0); ShadeNative.resource(8,0); ShadeNative.resource(9,0);
            close(normals); close(ids); close(table);
            normals=null; ids=null; table=null;
            System.out.println("[mcopt] materials: neutral fallback after atlas build failed: "+failure);
        }
    }

    private static void close(GpuTexture texture) { if (texture!=null) texture.close(); }
    private static int rank(TextureAtlasSprite sprite) {
        Identifier name=sprite.contents().name();
        return Policy.rank(name.getNamespace(),name.getPath());
    }

    private static Material readMaterial(Resource resource) throws Exception {
        try (InputStream in=resource.open(); NativeImage image=NativeImage.read(in)) {
            int iw=image.getWidth(),ih=image.getHeight();
            if (!Policy.staticMap(iw,ih,resource.metadata().getSection(net.minecraft.client.resources.metadata.animation.AnimationMetadataSection.TYPE).isPresent())) return null;
            byte[] pixels=new byte[SIZE*SIZE*4];
            int min=255,max=0;
            for (int y=0;y<SIZE;y++) for (int x=0;x<SIZE;x++) {
                int argb=image.getPixel(x*iw/SIZE,y*ih/SIZE),o=(y*SIZE+x)*4;
                pixels[o]=(byte)(argb>>>16); pixels[o+1]=(byte)(argb>>>8); pixels[o+2]=(byte)argb; pixels[o+3]=(byte)(argb>>>24);
                int a=argb>>>24; min=Math.min(min,a); max=Math.max(max,a);
            }
            return new Material(pixels,min,max);
        }
    }

    private static void build(Pending pending, GpuTexture[] output) throws Exception {
        SpriteLoader.Preparations atlas=pending.atlas();
        int aw=atlas.width(), ah=atlas.height();
        if (aw%GRID!=0 || ah%GRID!=0) throw new IllegalStateException("unaligned diffuse atlas "+aw+"x"+ah);
        List<TextureAtlasSprite> sprites=new ArrayList<>(atlas.regions().values());
        sprites.sort(Comparator.comparingInt(ShadeMaterials::rank).thenComparing(s->s.contents().name().toString()));
        List<Tile> tiles=new ArrayList<>();
        int realMaps=0, neutralTiles=0;
        for (TextureAtlasSprite sprite:sprites) {
            if (tiles.size()==MAX) break;
            Identifier name=sprite.contents().name();
            if (!name.getPath().startsWith("block/")) continue;
            int x0=Math.round(sprite.getU0()*aw), y0=Math.round(sprite.getV0()*ah);
            int x1=Math.round(sprite.getU1()*aw), y1=Math.round(sprite.getV1()*ah);
            // Stitched mip-aligned rectangles give the lookup exact coverage. Small/unusual
            // sprites deliberately remain neutral rather than colliding in one lookup cell.
            if (!Policy.rectangle(x0,y0,x1,y1,aw,ah)) continue;
            boolean solidSurface=Policy.solidSurface(name.getNamespace(),name.getPath());
            var resource=pending.resources().getResource(name.withPath("textures/"+name.getPath()+"_n.png"));
            var diffuse=pending.resources().getResource(name.withPath("textures/"+name.getPath()+".png"));
            // A geometry overlay may inherit the skin. A skin replacement without matching
            // normals must not inherit unrelated lower-priority material maps.
            Material material=null;
            if (Policy.mapCandidate(sprite.contents().isAnimated(),diffuse.map(Resource::sourcePackId).orElse(null),resource.map(Resource::sourcePackId).orElse(null))) {
                try { material=readMaterial(resource.get()); }
                catch (Exception badTile) { System.out.println("[mcopt] materials: skipped _n map for "+name+": "+badTile); }
            }
            // A missing, invalid, or wrong-pack map must not erase known terrain's ID.
            // It also must not classify arbitrary sprites with similar file names.
            if (material==null && !solidSurface) continue;
            boolean pom=material!=null && Policy.parallax(name.getPath());
            tiles.add(new Tile(sprite,material,x0,y0,x1,y1,pom,solidSurface));
            if (material==null) neutralTiles++; else realMaps++;
        }
        if (tiles.isEmpty()) {
            System.out.println("[mcopt] materials: no eligible terrain sprites or matching static LabPBR _n maps; neutral fallback");
            return;
        }
        Policy.Layout layout=Policy.layout(realMaps,neutralTiles>0);
        int width=layout.width(),height=layout.height(), gw=aw/GRID,gh=ah/GRID;
        ByteBuffer pixels=MemoryUtil.memAlloc(width*height*4);
        ByteBuffer grid=MemoryUtil.memCalloc(gw*gh);
        ByteBuffer bounds=MemoryUtil.memCalloc(TABLE*3*16).order(ByteOrder.nativeOrder());
        int heightMaps=0, nextCell=neutralTiles>0 ? 1 : 0;
        try {
            // Unused compact cells are neutral too, never uninitialized memory.
            Policy.neutralPixels(pixels);
            for (int k=0;k<tiles.size();k++) {
                Tile tile=tiles.get(k);
                int id=k+1, cell=-1, min=255,max=255;
                Material material=tile.material();
                if (material!=null) {
                    cell=nextCell++;
                    min=material.min(); max=material.max();
                    Policy.tilePixels(pixels,layout,cell,material.pixels());
                }
                TextureAtlasSprite sprite=tile.sprite();
                Policy.mapping(bounds,grid,id,aw,ah,tile.x0(),tile.y0(),tile.x1(),tile.y1(),sprite.getU0(),sprite.getV0(),sprite.getU1(),sprite.getV1(),
                    layout,cell,min,max,tile.pom(),tile.solidSurface());
                if (tile.pom() && min<max) heightMaps++;
            }
            output[0]=upload("mcopt LabPBR compact normal AO height",GpuFormat.RGBA8_UNORM,width,height,pixels);
            output[1]=upload("mcopt LabPBR exact sprite IDs",GpuFormat.R8_UNORM,gw,gh,grid);
            output[2]=upload("mcopt LabPBR sprite bounds",GpuFormat.RGBA32_FLOAT,TABLE,3,bounds);
            long bytes=(long)pixels.capacity()+grid.capacity()+bounds.capacity();
            System.out.println("[mcopt] materials: "+realMaps+" static _n tiles, "+neutralTiles+" neutral terrain IDs, "+heightMaps+" near-height tiles; compact "+width+"x"+height+
                ", diffuse "+aw+"x"+ah+", "+(bytes/1048576.0)+" MiB; normal "+STRENGTH+", parallax "+DEPTH);
        } finally { MemoryUtil.memFree(pixels); MemoryUtil.memFree(grid); MemoryUtil.memFree(bounds); }
    }

    private static GpuTexture upload(String label,GpuFormat format,int w,int h,ByteBuffer data) {
        GpuTexture texture=RenderSystem.getDevice().createTexture(label,GpuTexture.USAGE_TEXTURE_BINDING|GpuTexture.USAGE_COPY_DST,format,w,h,1,1);
        try {
            RenderSystem.getDevice().createCommandEncoder().writeToTexture(texture,data,0,0,0,0,w,h);
            return texture;
        } catch (RuntimeException failure) { texture.close(); throw failure; }
    }
}

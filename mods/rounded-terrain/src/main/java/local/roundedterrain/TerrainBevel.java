package local.roundedterrain;

import java.util.*;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.Predicate;
import local.roundedterrain.BevelMesh.Point;
import net.minecraft.client.Minecraft;
import net.caffeinemc.mods.sodium.client.model.quad.BakedQuadView;
import net.caffeinemc.mods.sodium.client.render.model.MutableQuadViewImpl;
import net.caffeinemc.mods.sodium.client.render.model.SodiumShadeMode;
import net.caffeinemc.mods.sodium.client.services.PlatformModelAccess;
import net.caffeinemc.mods.sodium.client.services.PlatformBlockAccess;
import net.caffeinemc.mods.sodium.client.services.PlatformModelEmitter;
import net.caffeinemc.mods.sodium.client.render.chunk.compile.pipeline.ShapeComparisonCache;
import net.minecraft.client.renderer.block.BlockAndTintGetter;
import net.minecraft.client.renderer.block.dispatch.BlockStateModel;
import net.minecraft.client.renderer.block.dispatch.BlockStateModelPart;
import net.minecraft.client.resources.model.geometry.BakedQuad;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.util.RandomSource;
import net.minecraft.util.TriState;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;

/** Selected full cubes only. Existing Sodium tint/light/AO/material and Metal terrain buffers own every emitted quad. */
public final class TerrainBevel {
    public static final boolean ENABLED=Boolean.parseBoolean(System.getProperty("roundedterrain.enabled","true"));
    public static final float RADIUS=radius();
    public static final boolean STATS=Boolean.getBoolean("roundedterrain.stats");
    public static final int CACHE_LIMIT=2048; // covers all 1,361 structurally valid exposed/end masks
    private static final Direction[] DIR={Direction.WEST,Direction.EAST,Direction.DOWN,Direction.UP,Direction.NORTH,Direction.SOUTH};
    private static final ConcurrentHashMap<Long,BevelMesh> MESHES=new ConcurrentHashMap<>();
    private static final AtomicLong ACCEPTED=new AtomicLong(),FALLBACK=new AtomicLong(),QUADS=new AtomicLong(),BASELINE=new AtomicLong();
    private static final Set<net.minecraft.world.level.block.Block> SEEN=ConcurrentHashMap.newKeySet();
    static { System.out.println("[roundedterrain] contextual mesh enabled="+ENABLED+", radius="+RADIUS+" blocks; MC26.3/Sodium0.9.3-alpha.1; collision unchanged"); }
    private TerrainBevel() {}
    private static float radius() {
        try { float r=Float.parseFloat(System.getProperty("roundedterrain.radius","0.04")); return Float.isFinite(r)?Math.clamp(r,0f,0.125f):0.04f; }
        catch(NumberFormatException e) { return 0.04f; }
    }
    private static boolean selected(BlockState s) {
        return s.is(Blocks.DIRT)||s.is(Blocks.COARSE_DIRT)||s.is(Blocks.ROOTED_DIRT)||s.is(Blocks.GRASS_BLOCK)
            ||s.is(Blocks.STONE)||s.is(Blocks.ANDESITE)||s.is(Blocks.SAND)||s.is(Blocks.RED_SAND)||s.is(Blocks.GRAVEL);
    }
    private static boolean hidden(BlockAndTintGetter view,BlockPos pos,int face) {
        Direction direction=DIR[face];
        BlockPos neighborPos=pos.relative(direction);
        BlockState s=view.getBlockState(pos),n=view.getBlockState(neighborPos);
        // Same order as Sodium0.9.3 shouldDrawSide: a full face occlusion shape hides first,
        // irrespective of canOcclude. For our full opaque source cube partial shapes cannot
        // hide the entire face; empty/non-occluding neighbors therefore continue to expose it.
        if(ShapeComparisonCache.isFullShape(n.getFaceOcclusionShape(direction.getOpposite()))) return true;
        return s.skipRendering(n,direction)||PlatformBlockAccess.getInstance().shouldSkipRender(view,s,n,pos,neighborPos,direction);
    }
    private static long continuing(BlockAndTintGetter view,BlockPos pos,int exposed) {
        long result=0;
        for(int a=0;a<6;a++) for(int b=a+1;b<6;b++) if(a/2!=b/2&&(exposed&(1<<a))!=0&&(exposed&(1<<b))!=0) {
            int free=3-a/2-b/2;
            for(int end=0;end<2;end++) {
                int endFace=free*2+end;
                if((exposed&(1<<endFace))!=0) continue; // free end: the local corner miter owns it
                BlockPos neighbor=pos.relative(DIR[endFace]);
                if(selected(view.getBlockState(neighbor))&&!hidden(view,neighbor,a)&&!hidden(view,neighbor,b))
                    result|=1L<<(BevelMesh.pair(a,b)*2+end);
            }
        }
        return result;
    }

    private static BevelMesh meshFor(BlockAndTintGetter view,BlockPos pos,int exposed) {
        long ends=continuing(view,pos,exposed),key=(ends<<6)|exposed;
        BevelMesh mesh=MESHES.get(key);
        if(mesh==null) {
            mesh=MESHES.size()<CACHE_LIMIT ? MESHES.computeIfAbsent(key,ignored->new BevelMesh(exposed,ends,RADIUS))
                : new BevelMesh(exposed,ends,RADIUS);
        }
        return mesh;
    }

    /** Selection geometry uses the same eligibility, neighbor masks and mesh cache as terrain emission. */
    public static BevelMesh outlineMesh(BlockAndTintGetter view,BlockPos pos,BlockState state) {
        if(!ENABLED||RADIUS==0||view==null||!selected(state)) return null;
        int exposed=0;
        for(int f=0;f<6;f++) if(!hidden(view,pos,f)) exposed|=1<<f;
        boolean curved=false;
        for(int a=0;a<6;a++) for(int b=a+1;b<6;b++)
            if(a/2!=b/2&&(exposed&(1<<a))!=0&&(exposed&(1<<b))!=0) curved=true;
        if(!curved) return null;
        BlockStateModel model=Minecraft.getInstance().getModelManager().getBlockStateModelSet().get(state);
        MutableQuadViewImpl quad=new MutableQuadViewImpl() { public void emitDirectly() {} };
        RandomSource random=RandomSource.create(state.getSeed(pos));
        List<BlockStateModelPart> parts=PlatformModelAccess.getInstance().collectPartsOf(model,view,pos,state,random,quad);
        if(parts.isEmpty()) return null;
        int covered=0;
        for(BlockStateModelPart part:parts) {
            if(!part.getQuads(null).isEmpty()) return null;
            for(int f=0;f<6;f++) {
                List<BakedQuad> qs=part.getQuads(DIR[f]);
                if(!qs.isEmpty()) covered|=1<<f;
                for(BakedQuad q:qs) if(!fullFace(q,f)) return null;
            }
        }
        return covered==63?meshFor(view,pos,exposed):null;
    }

    public static void emit(PlatformModelEmitter original,BlockStateModel model,Predicate<Direction> cull,
                            MutableQuadViewImpl quad,RandomSource random,BlockAndTintGetter view,
                            BlockPos pos,BlockState state,PlatformModelEmitter.Bufferer buffer) {
        if(!ENABLED||RADIUS==0||!selected(state)) { original.emitModel(model,cull,quad,random,view,pos,state,buffer); return; }
        int exposed=0;
        for(int f=0;f<6;f++) if(!cull.test(DIR[f])) exposed|=1<<f;
        boolean possibleBevel=false;
        for(int a=0;a<6;a++) for(int b=a+1;b<6;b++)
            if(a/2!=b/2&&(exposed&(1<<a))!=0&&(exposed&(1<<b))!=0) possibleBevel=true;
        if(!possibleBevel) {
            // The common flat-ground/buried case needs no mesh allocation, model capture or UV work.
            // Preserve unculled custom model details too instead of assuming a hidden cube is empty.
            if(STATS) BASELINE.incrementAndGet();
            original.emitModel(model,cull,quad,random,view,pos,state,buffer);
            return;
        }
        // Snapshot the model parts: this preserves randomized resource-pack choices and separates grass overlay layers.
        List<BlockStateModelPart> parts=new ArrayList<>(PlatformModelAccess.getInstance().collectPartsOf(model,view,pos,state,random,quad));
        List<List<List<BakedQuad>>> layers=new ArrayList<>(parts.size());
        boolean valid=!parts.isEmpty();
        int covered=0;
        for(BlockStateModelPart part:parts) {
            List<List<BakedQuad>> faces=new ArrayList<>(6);
            if(!part.getQuads(null).isEmpty()) valid=false;
            for(int f=0;f<6;f++) {
                List<BakedQuad> qs=part.getQuads(DIR[f]); faces.add(qs);
                if(!qs.isEmpty()) covered|=1<<f;
                for(BakedQuad q:qs) if(!fullFace(q,f)) valid=false;
            }
            layers.add(faces);
        }
        if(!valid||covered!=63) {
            long count=FALLBACK.incrementAndGet();
            if(count==1) System.out.println("[roundedterrain] preserved unsupported non-cube model: "+state+"; source parts="+parts.size());
            random.setSeed(state.getSeed(pos));
            original.emitModel(model,cull,quad,random,view,pos,state,buffer);
            return;
        }
        BevelMesh geometry=meshFor(view,pos,exposed);
        int emitted=0;
        for(int partIndex=0;partIndex<parts.size();partIndex++) {
            BlockStateModelPart part=parts.get(partIndex);
            // The default path sets the part's AO mode and loads its tint, source colors, layer, sprite and light flags.
            buffer.emit(part,cull,q->{ deformFace(q,geometry); q.emitDirectly(); });
            if(STATS) for(int f=0;f<6;f++) if((exposed&(1<<f))!=0) emitted+=layers.get(partIndex).get(f).size();
            for(BevelMesh.Patch patch:geometry.patches) if(!patch.kind().equals("face")) {
                for(BakedQuad source:layers.get(partIndex).get(patch.textureFace())) {
                    emitPatch(quad,source,patch,part.useAmbientOcclusion()); if(STATS) emitted++;
                }
            }
        }
        quad.clear();
        if(STATS) {
            long count=ACCEPTED.incrementAndGet(),total=QUADS.addAndGet(emitted);
            if(SEEN.add(state.getBlock())||count%10000==0) System.out.println("[roundedterrain] accepted blocks="+count+", emitted quads="+total+
                ", cached shapes="+MESHES.size()+", baseline="+BASELINE.get()+", unsupported="+FALLBACK.get()+", sample="+state+" at "+pos+
                ", exposed="+exposed+", geometric patches="+geometry.patches.size());
        }
    }

    private static boolean fullFace(BakedQuad q,int face) {
        int axis=face/2,corners=0;
        for(int i=0;i<4;i++) {
            var p=q.position(i);
            float[] v={p.x(),p.y(),p.z()};
            int bits=0;
            for(int a=0;a<3;a++) {
                if(Math.abs(v[a])<1e-5) {}
                else if(Math.abs(v[a]-1)<1e-5) bits|=1<<a;
                else return false;
            }
            if(((bits>>axis)&1)!=(face&1)) return false;
            int b=(axis+1)%3,c=(axis+2)%3;
            corners|=1<<(((bits>>b)&1)|(((bits>>c)&1)<<1));
        }
        var p0=q.position(0); var p1=q.position(1); var p3=q.position(3);
        float ax=p1.x()-p0.x(),ay=p1.y()-p0.y(),az=p1.z()-p0.z();
        float bx=p3.x()-p0.x(),by=p3.y()-p0.y(),bz=p3.z()-p0.z();
        return corners==15 && Math.abs(ax*ax+ay*ay+az*az-1)<1e-5 && Math.abs(bx*bx+by*by+bz*bz-1)<1e-5
            && Math.abs(ax*bx+ay*by+az*bz)<1e-5;
    }
    private static int faceIndex(Direction d) { for(int i=0;i<6;i++) if(DIR[i]==d) return i; throw new IllegalArgumentException("missing face"); }
    private record UV(float x,float y,float z,float ax,float ay,float az,float bx,float by,float bz,
                      float u,float v,float au,float av,float bu,float bv,float aa,float bb) {
        static UV from(BakedQuadView q) {
            float ax=q.getX(1)-q.getX(0),ay=q.getY(1)-q.getY(0),az=q.getZ(1)-q.getZ(0);
            float bx=q.getX(3)-q.getX(0),by=q.getY(3)-q.getY(0),bz=q.getZ(3)-q.getZ(0);
            return new UV(q.getX(0),q.getY(0),q.getZ(0),ax,ay,az,bx,by,bz,q.getTexU(0),q.getTexV(0),
                q.getTexU(1)-q.getTexU(0),q.getTexV(1)-q.getTexV(0),q.getTexU(3)-q.getTexU(0),q.getTexV(3)-q.getTexV(0),
                ax*ax+ay*ay+az*az,bx*bx+by*by+bz*bz);
        }
        static UV from(MutableQuadViewImpl q) {
            float ax=q.getX(1)-q.getX(0),ay=q.getY(1)-q.getY(0),az=q.getZ(1)-q.getZ(0);
            float bx=q.getX(3)-q.getX(0),by=q.getY(3)-q.getY(0),bz=q.getZ(3)-q.getZ(0);
            return new UV(q.getX(0),q.getY(0),q.getZ(0),ax,ay,az,bx,by,bz,q.getTexU(0),q.getTexV(0),
                q.getTexU(1)-q.getTexU(0),q.getTexV(1)-q.getTexV(0),q.getTexU(3)-q.getTexU(0),q.getTexV(3)-q.getTexV(0),
                ax*ax+ay*ay+az*az,bx*bx+by*by+bz*bz);
        }
        void apply(MutableQuadViewImpl q,int i,Point p) {
            float dx=p.x()-x,dy=p.y()-y,dz=p.z()-z;
            float a=(dx*ax+dy*ay+dz*az)/aa,b=(dx*bx+dy*by+dz*bz)/bb;
            q.setPos(i,p.x(),p.y(),p.z()).setUV(i,u+a*au+b*bu,v+a*av+b*bv);
        }
    }
    private static void deformFace(MutableQuadViewImpl q,BevelMesh mesh) {
        int face=faceIndex(q.getCullFace());
        UV uv=UV.from(q);
        Point[] p=new Point[4];
        for(int i=0;i<4;i++) { int bits=Math.round(q.getX(i))|(Math.round(q.getY(i))<<1)|(Math.round(q.getZ(i))<<2); p[i]=mesh.faceVertex(face,bits); }
        var sprite=q.cachedSprite();
        for(int i=0;i<4;i++) uv.apply(q,i,p[i]);
        q.cachedSprite(sprite);
    }
    private static void emitPatch(MutableQuadViewImpl q,BakedQuad source,BevelMesh.Patch patch,boolean ao) {
        q.fromBakedQuad(source);
        UV uv=UV.from((BakedQuadView)(Object)source);
        q.setCullFace(null).setNominalFace(null).setAmbientOcclusion(ao?TriState.DEFAULT:TriState.FALSE).setShadeMode(SodiumShadeMode.ENHANCED);
        Point[] p=patch.points();
        Point n=BevelMesh.cross(BevelMesh.subtract(p[2],p[0]),BevelMesh.subtract(p[3],p[1]));
        float inv=(float)(1/Math.sqrt(BevelMesh.dot(n,n)));
        Point[] normals=patch.normals();
        for(int i=0;i<4;i++) {
            uv.apply(q,i,p[i]);
            Point smooth=normals==null?null:normals[i];
            q.setNormal(i,smooth==null?n.x()*inv:smooth.x(),smooth==null?n.y()*inv:smooth.y(),smooth==null?n.z()*inv:smooth.z());
        }
        q.cachedSprite(source.materialInfo().sprite());
        q.emitDirectly();
    }
}

/* Machine application shaders. One invocation owns one pixel and resolves
 * triangles in draw order, keeping colour and depth together in registers.
 * Tile-local candidate binning uses shared atomics and barriers; framebuffer
 * colour/depth are still written by exactly one pixel invocation, so there
 * are no cross-workgroup colour/depth races or locks.
 * The built-in replacements are exact-match only. RenderWare's ordered
 * raster source is also recognized as a pixel-owned kernel, but is compiled
 * unchanged so its blend/depth contract remains intact. VERSION 3 marks the
 * pixel grid; older daemons reject it before submission.
 *
 * VERSION 8 adds a COARSE-BIN prepass contract: the daemon dispatches a
 * built-in binning kernel between xform and raster that appends each
 * triangle to every 64x64-pixel cell its bbox covers (bin SSBO below, one
 * [count][list] record per cell).  Each 16x16 tile lies wholly inside one
 * cell, so the tile scans only that cell's list instead of every triangle;
 * uBinCells == 0, an out-of-grid cell, or a count past QCOM8GEN3_BIN_CHUNK (list
 * overflow / binning disabled) falls back to the historic full scan.
 */
#ifndef ADRENO_GEOM_SOURCE_H
#define ADRENO_GEOM_SOURCE_H
#include <string.h>
#include <geom_raster_cs.h>
#include <rw_raster_cs.h>

#define QCOM8GEN3_GEOM_PIXEL_BODY \
"precision highp float;\n" \
"precision highp int;\n" \
"#ifndef QCOM8GEN3_TILE_W\n" \
"#define QCOM8GEN3_TILE_W 16\n" \
"#endif\n" \
"#ifndef QCOM8GEN3_TILE_H\n" \
"#define QCOM8GEN3_TILE_H 16\n" \
"#endif\n" \
"#define QCOM8GEN3_TILE_LANES (QCOM8GEN3_TILE_W*QCOM8GEN3_TILE_H)\n" \
"layout(local_size_x=QCOM8GEN3_TILE_W,local_size_y=QCOM8GEN3_TILE_H) in;\n" \
/* A larger shared candidate block reduces barrier/atomic phases for the
 * desktop mesh path (teapot is ~4k triangles at its normal tessellation),
 * while remaining well below the A690 workgroup shared-memory limit. */ \
"#define QCOM8GEN3_BIN_CHUNK 2048\n" \
"layout(std430,binding=0) buffer PglCsFb { uint fb[]; };\n" \
"layout(std430,binding=1) buffer PglCsVout { float vout[]; };\n" \
"layout(std430,binding=2) buffer PglCsDepth { uint depth[]; };\n" \
"#if QCOM8GEN3_TEXTURE\n" \
"layout(std430,binding=3) buffer PglCsTex { uint tex[]; };\n" \
"layout(std430,binding=4) buffer PglCsBin { uint bin[]; };\n" \
"#else\n" \
"layout(std430,binding=3) buffer PglCsBin { uint bin[]; };\n" \
"#endif\n" \
"uniform vec2 uViewportWH;\n" \
"uniform int uFbStride;\n" \
"uniform int uTriFirst;\n" \
"uniform int uTriCount;\n" \
"/* Coarse-bin grid extents (ceil(viewport/64)); 0 = no binning prepass ran\n" \
"   (old client or disabled) and every tile takes the full-scan path. */\n" \
"uniform ivec2 uBinCells;\n" \
"/* The client may launch only the primitives' bounding-box rectangle (the\n" \
"   daemon shrinks the raster grid to that window); uDispatchOffset remaps\n" \
"   the window-relative global IDs back to framebuffer coordinates.  Zero\n" \
"   keeps the historic full-frame launch. */\n" \
"uniform ivec2 uDispatchOffset;\n" \
"#if QCOM8GEN3_TEXTURE\n" \
"uniform int uTexW;\n" \
"uniform int uTexH;\n" \
"#else\n" \
"uniform int uBlendMode;\n" \
"uniform int uDepthWrite;\n" \
"uniform int uCullSign;\n" \
"#endif\n" \
"float edgef(vec2 a,vec2 b,vec2 p) {\n" \
" return (p.x-a.x)*(b.y-a.y)-(p.y-a.y)*(b.x-a.x);\n" \
"}\n" \
"vec2 screen(int i) { return vec2(vout[i*8],vout[i*8+1]); }\n" \
"#ifndef QCOM8GEN3_TRI_CACHE\n" \
"#define QCOM8GEN3_TRI_CACHE (!QCOM8GEN3_TEXTURE)\n" \
"#endif\n" \
"#if QCOM8GEN3_TRI_CACHE\n" \
"shared float qcom8gen3_tri_cache[64*24];\n" \
"#define QCOM8GEN3_VOUT qcom8gen3_tri_cache\n" \
"#else\n" \
"#define QCOM8GEN3_VOUT vout\n" \
"#endif\n" \
"vec2 raster_screen(int i) { return vec2(QCOM8GEN3_VOUT[i*8],QCOM8GEN3_VOUT[i*8+1]); }\n" \
"shared uint qcom8gen3_bin_count;\n" \
"shared uint qcom8gen3_bin[QCOM8GEN3_BIN_CHUNK];\n" \
"bool touches_tile(int t, vec2 tile_lo, vec2 tile_hi) {\n" \
" int i0=(uTriFirst+t)*3,i1=i0+1,i2=i0+2;\n" \
" vec3 iw=vec3(vout[i0*8+3],vout[i1*8+3],vout[i2*8+3]);\n" \
" if(any(lessThanEqual(iw,vec3(0.0)))) return false;\n" \
" vec2 s0=screen(i0),s1=screen(i1),s2=screen(i2);\n" \
" float area=edgef(s1,s2,s0);\n" \
" if(area==0.0) return false;\n" \
" vec2 lo=min(s0,min(s1,s2)),hi=max(s0,max(s1,s2));\n" \
" if(hi.x<tile_lo.x||lo.x>tile_hi.x||hi.y<tile_lo.y||lo.y>tile_hi.y) return false;\n" \
"#if !QCOM8GEN3_TEXTURE\n" \
" if(uCullSign==3||(uCullSign==1&&area>=0.0)||(uCullSign==2&&area<=0.0)) return false;\n" \
"#endif\n" \
" return true;\n" \
"}\n" \
"void main() {\n" \
" ivec2 xy=ivec2(gl_GlobalInvocationID.xy)+uDispatchOffset;\n" \
" bool pixel_active=xy.x<int(uViewportWH.x)&&xy.y<int(uViewportWH.y);\n" \
" int lid=int(gl_LocalInvocationID.y)*QCOM8GEN3_TILE_W+int(gl_LocalInvocationID.x);\n" \
" vec2 tile_lo=vec2(gl_WorkGroupID.xy)*vec2(float(QCOM8GEN3_TILE_W),float(QCOM8GEN3_TILE_H))+vec2(uDispatchOffset);\n" \
" vec2 tile_hi=tile_lo+vec2(float(QCOM8GEN3_TILE_W),float(QCOM8GEN3_TILE_H));\n" \
" uint bestDepth=0xffffffffu,bestColor=0u; bool dirty=false;\n" \
" if(pixel_active) { int idx=xy.y*uFbStride+xy.x; bestDepth=depth[idx]; bestColor=fb[idx]; }\n" \
" vec2 p=vec2(xy)+vec2(0.5);\n" \
" /* Coarse-bin candidate source: this tile's 64x64 cell list when the\n" \
"    prepass filled it, else the full triangle range.  A 16x16 tile never\n" \
"    straddles a 64-pixel cell boundary, so the tile origin picks the cell.\n" \
"    count > QCOM8GEN3_BIN_CHUNK means the list overflowed (or binning is off:\n" \
"    the daemon seeds counts with 0xffffffff) -> full scan keeps every\n" \
"    triangle visible.  Shifts only: no integer division on this GPU. */\n" \
" int ncand=uTriCount;\n" \
" uint bin_base=0u;\n" \
" bool use_bin=false;\n" \
" if(uBinCells.x>0&&uBinCells.y>0) {\n" \
"  ivec2 cell=ivec2(tile_lo)>>6;\n" \
"  if(cell.x<uBinCells.x&&cell.y<uBinCells.y) {\n" \
"   uint cc=bin[uint(cell.y*uBinCells.x+cell.x)*(uint(QCOM8GEN3_BIN_CHUNK)+1u)];\n" \
"   if(cc>0u&&cc<=uint(QCOM8GEN3_BIN_CHUNK)) {\n" \
"    use_bin=true; ncand=int(cc);\n" \
"    bin_base=uint(cell.y*uBinCells.x+cell.x)*(uint(QCOM8GEN3_BIN_CHUNK)+1u)+1u;\n" \
"   }\n" \
"  }\n" \
" }\n" \
" for(int chunk=0;chunk<ncand;chunk+=QCOM8GEN3_BIN_CHUNK) {\n" \
"  if(lid==0) qcom8gen3_bin_count=0u;\n" \
"  barrier();\n" \
"  for(int t=chunk+lid;t<min(chunk+QCOM8GEN3_BIN_CHUNK,ncand);t+=QCOM8GEN3_TILE_LANES) {\n" \
"   int tri=use_bin?int(bin[bin_base+uint(t)]):t;\n" \
"   if(touches_tile(tri,tile_lo,tile_hi)) { uint s=atomicAdd(qcom8gen3_bin_count,1u); qcom8gen3_bin[s]=uint(tri); }\n" \
"  }\n" \
"  barrier();\n" \
"  uint nc=min(qcom8gen3_bin_count,uint(QCOM8GEN3_BIN_CHUNK));\n" \
"#if !QCOM8GEN3_TEXTURE\n" \
"  /* Atomic append order is intentionally unspecified.  Opaque depth tests\n" \
"   * are order independent, but source-over/additive blending is not; sort\n" \
"   * only that slower path back into the original triangle order. */\n" \
"  if(uBlendMode!=0 && lid==0) {\n" \
"   for(uint si=1u;si<nc;si++) { uint key=qcom8gen3_bin[si]; int sj=int(si);\n" \
"    while(sj>0 && qcom8gen3_bin[uint(sj)-1u]>key) { qcom8gen3_bin[uint(sj)]=qcom8gen3_bin[uint(sj)-1u]; sj--; }\n" \
"    qcom8gen3_bin[uint(sj)]=key;\n" \
"   }\n" \
"  }\n" \
"#endif\n" \
"  barrier();\n" \
"#if QCOM8GEN3_TRI_CACHE\n" \
"  for(uint cb=0u;cb<nc;cb+=64u) {\n" \
"   uint bn=min(64u,nc-cb);\n" \
"   for(uint k=uint(lid);k<bn*24u;k+=uint(QCOM8GEN3_TILE_LANES)) {\n" \
"    int t=int(qcom8gen3_bin[cb+k/24u]);\n" \
"    qcom8gen3_tri_cache[k]=vout[(uTriFirst+t)*24+int(k%24u)];\n" \
"   }\n" \
"   barrier();\n" \
"   if(pixel_active) for(uint bi=0u;bi<bn;bi++) {\n" \
"    int i0=int(bi)*3,i1=i0+1,i2=i0+2;\n" \
"#else\n" \
"  if(pixel_active) for(uint bi=0u;bi<nc;bi++) {\n" \
"   int t=int(qcom8gen3_bin[bi]);\n" \
"   int i0=(uTriFirst+t)*3,i1=i0+1,i2=i0+2;\n" \
"#endif\n" \
"   vec3 iw=vec3(QCOM8GEN3_VOUT[i0*8+3],QCOM8GEN3_VOUT[i1*8+3],QCOM8GEN3_VOUT[i2*8+3]);\n" \
"   vec2 s0=raster_screen(i0),s1=raster_screen(i1),s2=raster_screen(i2);\n" \
"   float area=edgef(s1,s2,s0);\n" \
"   if(area==0.0) continue;\n" \
"  vec3 e=vec3(edgef(s1,s2,p),edgef(s2,s0,p),edgef(s0,s1,p));\n" \
"  if(any(lessThan(e*sign(area),vec3(0.0)))) continue;\n" \
"  vec3 b=e/area;\n" \
"  float invw=dot(b,iw);\n" \
"  if(invw<=0.0) continue;\n" \
"  vec3 q=b*iw/invw;\n" \
"  float z=dot(q,vec3(QCOM8GEN3_VOUT[i0*8+2],QCOM8GEN3_VOUT[i1*8+2],QCOM8GEN3_VOUT[i2*8+2]));\n" \
"  uint d=uint(clamp(z,0.0,1.0)*16777215.0);\n" \
"  if(d>=bestDepth) continue;\n" \
"#if QCOM8GEN3_TEXTURE\n" \
"  vec2 uv=q.x*vec2(QCOM8GEN3_VOUT[i0*8+4],QCOM8GEN3_VOUT[i0*8+5])+\n" \
"          q.y*vec2(QCOM8GEN3_VOUT[i1*8+4],QCOM8GEN3_VOUT[i1*8+5])+\n" \
"          q.z*vec2(QCOM8GEN3_VOUT[i2*8+4],QCOM8GEN3_VOUT[i2*8+5]);\n" \
"  int tx=clamp(int(uv.x*float(uTexW)),0,uTexW-1);\n" \
"  int ty=clamp(int((1.0-uv.y)*float(uTexH)),0,uTexH-1);\n" \
"  bestColor=tex[ty*uTexW+tx];\n" \
"  bestDepth=d;\n" \
"#else\n" \
"  vec3 col=q.x*vec3(QCOM8GEN3_VOUT[i0*8+4],QCOM8GEN3_VOUT[i0*8+5],QCOM8GEN3_VOUT[i0*8+6])+\n" \
"           q.y*vec3(QCOM8GEN3_VOUT[i1*8+4],QCOM8GEN3_VOUT[i1*8+5],QCOM8GEN3_VOUT[i1*8+6])+\n" \
"           q.z*vec3(QCOM8GEN3_VOUT[i2*8+4],QCOM8GEN3_VOUT[i2*8+5],QCOM8GEN3_VOUT[i2*8+6]);\n" \
"  col=clamp(col,0.0,1.0);\n" \
"  if(uBlendMode!=0) {\n" \
"   float a=clamp(dot(q,vec3(QCOM8GEN3_VOUT[i0*8+7],QCOM8GEN3_VOUT[i1*8+7],QCOM8GEN3_VOUT[i2*8+7])),0.0,1.0);\n" \
"   vec3 dst=vec3(float((bestColor>>16)&255u),float((bestColor>>8)&255u),float(bestColor&255u))/255.0;\n" \
"   col=uBlendMode==2?dst+col*a:col*a+dst*(1.0-a);\n" \
"  }\n" \
"  uvec3 rgb=uvec3(clamp(col,0.0,1.0)*255.0+0.5);\n" \
"  bestColor=0xff000000u|(rgb.r<<16)|(rgb.g<<8)|rgb.b;\n" \
"  if(uDepthWrite!=0) bestDepth=d;\n" \
"#endif\n" \
"   dirty=true;\n" \
"  }\n" \
"#if QCOM8GEN3_TRI_CACHE\n" \
"   barrier();\n" \
"  }\n" \
"#endif\n" \
"  barrier();\n" \
" }\n" \
" if(pixel_active && dirty) { int idx=xy.y*uFbStride+xy.x; fb[idx]=bestColor;\n" \
"#if QCOM8GEN3_TEXTURE\n" \
"  depth[idx]=bestDepth;\n" \
"#else\n" \
"  if(uDepthWrite!=0) depth[idx]=bestDepth;\n" \
"#endif\n" \
" }\n" \
"}\n"

static const char qcom8gen3_geom_color_pixel[] = "#version 310 es\n#define QCOM8GEN3_TEXTURE 0\n" QCOM8GEN3_GEOM_PIXEL_BODY;
static const char qcom8gen3_geom_texture_pixel[] = "#version 310 es\n#define QCOM8GEN3_TEXTURE 1\n" QCOM8GEN3_GEOM_PIXEL_BODY;
#undef QCOM8GEN3_GEOM_PIXEL_BODY

static const char *adreno_geom_source(const char *src) {
    if(!src) return NULL;
    if(!strcmp(src,geom_raster_cs_glsl)) return qcom8gen3_geom_color_pixel;
    if(!strcmp(src,geom_raster_tex_cs_glsl)) return qcom8gen3_geom_texture_pixel;
    return NULL;
}
#endif

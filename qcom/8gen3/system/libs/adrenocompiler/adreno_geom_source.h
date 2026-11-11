/* Machine application shaders. One invocation owns one pixel and resolves
 * triangles in draw order, keeping colour and depth together in registers.
 * Tile-local candidate binning uses shared atomics and barriers; framebuffer
 * colour/depth are still written by exactly one pixel invocation, so there
 * are no cross-workgroup colour/depth races or locks.
 * The built-in replacements are exact-match only. RenderWare's ordered
 * raster source is also recognized as a pixel-owned kernel, but is compiled
 * unchanged so its blend/depth contract remains intact. VERSION 3 marks the
 * pixel grid; older daemons reject it before submission.
 */
#ifndef ADRENO_GEOM_SOURCE_H
#define ADRENO_GEOM_SOURCE_H
#include <string.h>
#include <geom_raster_cs.h>
#include <rw_raster_cs.h>

#define Q8B_GEOM_PIXEL_BODY \
"precision highp float;\n" \
"precision highp int;\n" \
"#ifndef Q8B_TILE_W\n" \
"#define Q8B_TILE_W 16\n" \
"#endif\n" \
"#ifndef Q8B_TILE_H\n" \
"#define Q8B_TILE_H 16\n" \
"#endif\n" \
"#define Q8B_TILE_LANES (Q8B_TILE_W*Q8B_TILE_H)\n" \
"layout(local_size_x=Q8B_TILE_W,local_size_y=Q8B_TILE_H) in;\n" \
/* A larger shared candidate block reduces barrier/atomic phases for the
 * desktop mesh path (teapot is ~4k triangles at its normal tessellation),
 * while remaining well below the A690 workgroup shared-memory limit. */ \
"#define Q8B_BIN_CHUNK 2048\n" \
"layout(std430,binding=0) buffer PglCsFb { uint fb[]; };\n" \
"layout(std430,binding=1) buffer PglCsVout { float vout[]; };\n" \
"layout(std430,binding=2) buffer PglCsDepth { uint depth[]; };\n" \
"#if Q8B_TEXTURE\n" \
"layout(std430,binding=3) buffer PglCsTex { uint tex[]; };\n" \
"#endif\n" \
"uniform vec2 uViewportWH;\n" \
"uniform int uFbStride;\n" \
"uniform int uTriFirst;\n" \
"uniform int uTriCount;\n" \
"#if Q8B_TEXTURE\n" \
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
"#ifndef Q8B_TRI_CACHE\n" \
"#define Q8B_TRI_CACHE (!Q8B_TEXTURE)\n" \
"#endif\n" \
"#if Q8B_TRI_CACHE\n" \
"shared float q8b_tri_cache[64*24];\n" \
"#define Q8B_VOUT q8b_tri_cache\n" \
"#else\n" \
"#define Q8B_VOUT vout\n" \
"#endif\n" \
"vec2 raster_screen(int i) { return vec2(Q8B_VOUT[i*8],Q8B_VOUT[i*8+1]); }\n" \
"shared uint q8b_bin_count;\n" \
"shared uint q8b_bin[Q8B_BIN_CHUNK];\n" \
"bool touches_tile(int t, vec2 tile_lo, vec2 tile_hi) {\n" \
" int i0=(uTriFirst+t)*3,i1=i0+1,i2=i0+2;\n" \
" vec3 iw=vec3(vout[i0*8+3],vout[i1*8+3],vout[i2*8+3]);\n" \
" if(any(lessThanEqual(iw,vec3(0.0)))) return false;\n" \
" vec2 s0=screen(i0),s1=screen(i1),s2=screen(i2);\n" \
" float area=edgef(s1,s2,s0);\n" \
" if(area==0.0) return false;\n" \
" vec2 lo=min(s0,min(s1,s2)),hi=max(s0,max(s1,s2));\n" \
" if(hi.x<tile_lo.x||lo.x>tile_hi.x||hi.y<tile_lo.y||lo.y>tile_hi.y) return false;\n" \
"#if !Q8B_TEXTURE\n" \
" if(uCullSign==3||(uCullSign==1&&area>=0.0)||(uCullSign==2&&area<=0.0)) return false;\n" \
"#endif\n" \
" return true;\n" \
"}\n" \
"void main() {\n" \
" ivec2 xy=ivec2(gl_GlobalInvocationID.xy);\n" \
" bool pixel_active=xy.x<int(uViewportWH.x)&&xy.y<int(uViewportWH.y);\n" \
" int lid=int(gl_LocalInvocationID.y)*Q8B_TILE_W+int(gl_LocalInvocationID.x);\n" \
" vec2 tile_lo=vec2(gl_WorkGroupID.xy)*vec2(float(Q8B_TILE_W),float(Q8B_TILE_H));\n" \
" vec2 tile_hi=tile_lo+vec2(float(Q8B_TILE_W),float(Q8B_TILE_H));\n" \
" uint bestDepth=0xffffffffu,bestColor=0u; bool dirty=false;\n" \
" if(pixel_active) { int idx=xy.y*uFbStride+xy.x; bestDepth=depth[idx]; bestColor=fb[idx]; }\n" \
" vec2 p=vec2(xy)+vec2(0.5);\n" \
" for(int chunk=0;chunk<uTriCount;chunk+=Q8B_BIN_CHUNK) {\n" \
"  if(lid==0) q8b_bin_count=0u;\n" \
"  barrier();\n" \
"  for(int t=chunk+lid;t<min(chunk+Q8B_BIN_CHUNK,uTriCount);t+=Q8B_TILE_LANES) {\n" \
"   if(touches_tile(t,tile_lo,tile_hi)) { uint s=atomicAdd(q8b_bin_count,1u); q8b_bin[s]=uint(t); }\n" \
"  }\n" \
"  barrier();\n" \
"  uint nc=min(q8b_bin_count,uint(Q8B_BIN_CHUNK));\n" \
"#if !Q8B_TEXTURE\n" \
"  /* Atomic append order is intentionally unspecified.  Opaque depth tests\n" \
"   * are order independent, but source-over/additive blending is not; sort\n" \
"   * only that slower path back into the original triangle order. */\n" \
"  if(uBlendMode!=0 && lid==0) {\n" \
"   for(uint si=1u;si<nc;si++) { uint key=q8b_bin[si]; int sj=int(si);\n" \
"    while(sj>0 && q8b_bin[uint(sj)-1u]>key) { q8b_bin[uint(sj)]=q8b_bin[uint(sj)-1u]; sj--; }\n" \
"    q8b_bin[uint(sj)]=key;\n" \
"   }\n" \
"  }\n" \
"#endif\n" \
"  barrier();\n" \
"#if Q8B_TRI_CACHE\n" \
"  for(uint cb=0u;cb<nc;cb+=64u) {\n" \
"   uint bn=min(64u,nc-cb);\n" \
"   for(uint k=uint(lid);k<bn*24u;k+=uint(Q8B_TILE_LANES)) {\n" \
"    int t=int(q8b_bin[cb+k/24u]);\n" \
"    q8b_tri_cache[k]=vout[(uTriFirst+t)*24+int(k%24u)];\n" \
"   }\n" \
"   barrier();\n" \
"   if(pixel_active) for(uint bi=0u;bi<bn;bi++) {\n" \
"    int i0=int(bi)*3,i1=i0+1,i2=i0+2;\n" \
"#else\n" \
"  if(pixel_active) for(uint bi=0u;bi<nc;bi++) {\n" \
"   int t=int(q8b_bin[bi]);\n" \
"   int i0=(uTriFirst+t)*3,i1=i0+1,i2=i0+2;\n" \
"#endif\n" \
"   vec3 iw=vec3(Q8B_VOUT[i0*8+3],Q8B_VOUT[i1*8+3],Q8B_VOUT[i2*8+3]);\n" \
"   vec2 s0=raster_screen(i0),s1=raster_screen(i1),s2=raster_screen(i2);\n" \
"   float area=edgef(s1,s2,s0);\n" \
"   if(area==0.0) continue;\n" \
"  vec3 e=vec3(edgef(s1,s2,p),edgef(s2,s0,p),edgef(s0,s1,p));\n" \
"  if(any(lessThan(e*sign(area),vec3(0.0)))) continue;\n" \
"  vec3 b=e/area;\n" \
"  float invw=dot(b,iw);\n" \
"  if(invw<=0.0) continue;\n" \
"  vec3 q=b*iw/invw;\n" \
"  float z=dot(q,vec3(Q8B_VOUT[i0*8+2],Q8B_VOUT[i1*8+2],Q8B_VOUT[i2*8+2]));\n" \
"  uint d=uint(clamp(z,0.0,1.0)*16777215.0);\n" \
"  if(d>=bestDepth) continue;\n" \
"#if Q8B_TEXTURE\n" \
"  vec2 uv=q.x*vec2(Q8B_VOUT[i0*8+4],Q8B_VOUT[i0*8+5])+\n" \
"          q.y*vec2(Q8B_VOUT[i1*8+4],Q8B_VOUT[i1*8+5])+\n" \
"          q.z*vec2(Q8B_VOUT[i2*8+4],Q8B_VOUT[i2*8+5]);\n" \
"  int tx=clamp(int(uv.x*float(uTexW)),0,uTexW-1);\n" \
"  int ty=clamp(int((1.0-uv.y)*float(uTexH)),0,uTexH-1);\n" \
"  bestColor=tex[ty*uTexW+tx];\n" \
"  bestDepth=d;\n" \
"#else\n" \
"  vec3 col=q.x*vec3(Q8B_VOUT[i0*8+4],Q8B_VOUT[i0*8+5],Q8B_VOUT[i0*8+6])+\n" \
"           q.y*vec3(Q8B_VOUT[i1*8+4],Q8B_VOUT[i1*8+5],Q8B_VOUT[i1*8+6])+\n" \
"           q.z*vec3(Q8B_VOUT[i2*8+4],Q8B_VOUT[i2*8+5],Q8B_VOUT[i2*8+6]);\n" \
"  col=clamp(col,0.0,1.0);\n" \
"  if(uBlendMode!=0) {\n" \
"   float a=clamp(dot(q,vec3(Q8B_VOUT[i0*8+7],Q8B_VOUT[i1*8+7],Q8B_VOUT[i2*8+7])),0.0,1.0);\n" \
"   vec3 dst=vec3(float((bestColor>>16)&255u),float((bestColor>>8)&255u),float(bestColor&255u))/255.0;\n" \
"   col=uBlendMode==2?dst+col*a:col*a+dst*(1.0-a);\n" \
"  }\n" \
"  uvec3 rgb=uvec3(clamp(col,0.0,1.0)*255.0+0.5);\n" \
"  bestColor=0xff000000u|(rgb.r<<16)|(rgb.g<<8)|rgb.b;\n" \
"  if(uDepthWrite!=0) bestDepth=d;\n" \
"#endif\n" \
"   dirty=true;\n" \
"  }\n" \
"#if Q8B_TRI_CACHE\n" \
"   barrier();\n" \
"  }\n" \
"#endif\n" \
"  barrier();\n" \
" }\n" \
" if(pixel_active && dirty) { int idx=xy.y*uFbStride+xy.x; fb[idx]=bestColor;\n" \
"#if Q8B_TEXTURE\n" \
"  depth[idx]=bestDepth;\n" \
"#else\n" \
"  if(uDepthWrite!=0) depth[idx]=bestDepth;\n" \
"#endif\n" \
" }\n" \
"}\n"

static const char q8b_geom_color_pixel[] = "#version 310 es\n#define Q8B_TEXTURE 0\n" Q8B_GEOM_PIXEL_BODY;
static const char q8b_geom_texture_pixel[] = "#version 310 es\n#define Q8B_TEXTURE 1\n" Q8B_GEOM_PIXEL_BODY;
#undef Q8B_GEOM_PIXEL_BODY

static const char *adreno_geom_source(const char *src) {
    if(!src) return NULL;
    if(!strcmp(src,geom_raster_cs_glsl)) return q8b_geom_color_pixel;
    if(!strcmp(src,geom_raster_tex_cs_glsl)) return q8b_geom_texture_pixel;
    return NULL;
}
#endif

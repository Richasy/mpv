cbuffer cb : register(b0) { uint4 Const0; };
Texture2D InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);
#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p){return InputTexture.Load(int3(ASU2(p),0));}
void FsrRcasInputF(inout AF1 r,inout AF1 g,inout AF1 b){}
#include "ffx_fsr1.h"
[numthreads(64,1,1)]
void main(uint3 LocalThreadId:SV_GroupThreadID,uint3 WorkGroupId:SV_GroupID){
  AU2 gxy=ARmp8x8(LocalThreadId.x)+AU2(WorkGroupId.x<<4u,WorkGroupId.y<<4u);
  AF3 c;
  FsrRcasF(c.r,c.g,c.b,gxy,Const0);OutputTexture[gxy]=float4(c,1);
  gxy.x+=8u;
  FsrRcasF(c.r,c.g,c.b,gxy,Const0);OutputTexture[gxy]=float4(c,1);
  gxy.y+=8u;
  FsrRcasF(c.r,c.g,c.b,gxy,Const0);OutputTexture[gxy]=float4(c,1);
  gxy.x-=8u;
  FsrRcasF(c.r,c.g,c.b,gxy,Const0);OutputTexture[gxy]=float4(c,1);
}

cbuffer cb : register(b0) { uint4 Const0; uint4 Const1; uint4 Const2; uint4 Const3; };
SamplerState samLinearClamp : register(s0);
Texture2D InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);
#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p){return InputTexture.GatherRed(samLinearClamp,p,int2(0,0));}
AF4 FsrEasuGF(AF2 p){return InputTexture.GatherGreen(samLinearClamp,p,int2(0,0));}
AF4 FsrEasuBF(AF2 p){return InputTexture.GatherBlue(samLinearClamp,p,int2(0,0));}
#include "ffx_fsr1.h"
[numthreads(64,1,1)]
void main(uint3 LocalThreadId:SV_GroupThreadID,uint3 WorkGroupId:SV_GroupID){
  AU2 gxy=ARmp8x8(LocalThreadId.x)+AU2(WorkGroupId.x<<4u,WorkGroupId.y<<4u);
  AF3 c;
  FsrEasuF(c,gxy,Const0,Const1,Const2,Const3);OutputTexture[gxy]=float4(c,1);
  gxy.x+=8u;
  FsrEasuF(c,gxy,Const0,Const1,Const2,Const3);OutputTexture[gxy]=float4(c,1);
  gxy.y+=8u;
  FsrEasuF(c,gxy,Const0,Const1,Const2,Const3);OutputTexture[gxy]=float4(c,1);
  gxy.x-=8u;
  FsrEasuF(c,gxy,Const0,Const1,Const2,Const3);OutputTexture[gxy]=float4(c,1);
}

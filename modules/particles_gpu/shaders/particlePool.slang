#include "../../../assets/shaders/FogCommon.slang"

struct Particle { float4 positionAge,velocityLifetime,sizeRotation,color; uint4 identity; };
struct RibbonVertexData { float4 position,color,uv; };
struct View
{
    float4x4 viewProj,inverseViewProj;
    float4 anchorStart,right,up,sampling,rect;
    float4 sunDirection,sunColor,ambient,fogColor,fogRange,material;
    uint4 options;
    float4 texture;
};
[[vk::binding(1)]] Sampler2D<float4> depth;
[[vk::binding(5)]] StructuredBuffer<Particle> particleRead;
[[vk::binding(7)]] Sampler2D<float4> sprite;
[[vk::binding(8)]] StructuredBuffer<RibbonVertexData> ribbonVertices;
[[vk::binding(9)]] StructuredBuffer<uint2> sortedIndices;
[[vk::binding(10)]] StructuredBuffer<View> views;
struct Draw { uint index; };
[[vk::push_constant]] Draw draw;
struct Varying
{
    float4 position:SV_Position;
    float2 uv:TEXCOORD0;
    float4 color:COLOR0;
    float3 localPosition:TEXCOORD1;
    nointerpolation uint grid:TEXCOORD2;
    nointerpolation uint frame:TEXCOORD3;
};
float3 SafeNormal(float3 value,float3 fallback)
{
    float n=dot(value,value);
    return isfinite(n) && n>1e-10 ? value*rsqrt(n) : fallback;
}
[shader("vertex")]
Varying vertexMain(uint vertex:SV_VertexID,uint instance:SV_InstanceID)
{
    View view=views[draw.index];
    const float2 corners[6]={float2(-1,-1),float2(1,-1),float2(1,1),float2(-1,-1),float2(1,1),float2(-1,1)};
    uint index=sortedIndices[uint(view.sampling.w)+uint(view.anchorStart.w)*2+instance].y;
    Particle p=particleRead[index];
    float3 center=view.anchorStart.xyz+p.positionAge.xyz;
    float3 right=view.right.xyz,up=view.up.xyz;
    float3 normal=SafeNormal(cross(right,up),float3(0,0,1));
    if(view.options.y!=0)
    {
        float3 positionNormal=SafeNormal(-center,normal);
        normal=dot(positionNormal,normal)<0 ? -positionNormal : positionNormal;
        right=SafeNormal(cross(up,normal),right);
        up=SafeNormal(cross(normal,right),up);
    }
    if(view.options.x!=0)
    {
        up=SafeNormal(p.velocityLifetime.xyz-normal*dot(p.velocityLifetime.xyz,normal),up);
        right=SafeNormal(cross(up,normal),right);
    }
    float2 corner=corners[vertex];
    float3 position=center+(right*corner.x+up*corner.y)*p.sizeRotation.x;
    Varying o;
    o.position=mul(view.viewProj,float4(position,1));o.localPosition=position;
    o.uv=corner;o.color=p.color;
    if((p.identity.w&1)==0) o.color.a*=saturate(1-p.positionAge.w/p.velocityLifetime.w);
    o.grid=asuint(p.sizeRotation.w);o.frame=uint(p.sizeRotation.z);
    return o;
}
[shader("vertex")]
Varying ribbonVertex(uint vertex:SV_VertexID)
{
    View view=views[draw.index];
    RibbonVertexData p=ribbonVertices[vertex];
    Varying o;
    o.position=mul(view.viewProj,p.position);o.localPosition=p.position.xyz;
    o.uv=p.uv.xy;o.color=p.color;o.grid=o.frame=0;
    return o;
}
float4 ShadeParticle(Varying i,bool ribbon)
{
    View view=views[draw.index];
    float distance=length(i.localPosition);
    float2 pixel=clamp(i.position.xy-float2(view.right.w,view.up.w),view.rect.xy+0.5,view.rect.xy+view.rect.zw-0.5);
    float d=depth.Sample(pixel*view.sampling.xy).r;
    float soft=1;
    if(d>0)
    {
        float opaqueDistance=d;
        if(view.sampling.z==0)
        {
            float2 ndc=(pixel-view.rect.xy)/view.rect.zw*2-1;
            float4 opaque=mul(view.inverseViewProj,float4(ndc,d,1));
            opaqueDistance=length(opaque.xyz/opaque.w);
        }
        float gap=opaqueDistance-distance;
        if(gap<0) discard;
        if(view.material.x>0) soft=saturate(gap/view.material.x);
    }
    float edge=ribbon?abs(i.uv.y):length(i.uv);
    float aa=max(fwidth(edge),0.001);
    float alpha=i.color.a*soft*(1-smoothstep(1-aa,1+aa,edge));
    alpha*=1-smoothstep(ribbon?0.3:0.65,1,edge);
    float4 texel=1;
    if(view.texture.x!=0 || i.grid!=0)
    {
        float2 uv=ribbon?float2(i.uv.x,i.uv.y*0.5+0.5):i.uv*0.5+0.5;
        if(i.grid!=0)
        {
            uint columns=i.grid&65535,rows=i.grid>>16;
            float2 grid=float2(columns,rows);
            float2 cell=float2(i.frame%columns,i.frame/columns)/grid;
            uv=cell+lerp(view.texture.yz*0.5,1.0/grid-view.texture.yz*0.5,saturate(uv));
        }
        texel=sprite.Sample(uv);
    }
    alpha*=texel.a;
    float3 color=i.color.rgb*texel.rgb;
    if(view.material.z!=0)
    {
        float3 normal=SafeNormal(cross(ddx(i.localPosition),ddy(i.localPosition)),float3(0,0,1));
        float diffuse=abs(dot(normal,SafeNormal(view.sunDirection.xyz,float3(0,1,0))));
        color*=view.ambient.rgb+view.sunColor.rgb*diffuse;
    }
    if(view.material.w!=0)
    {
        float transmittance=DistanceFogTransmittance(distance,view.fogColor.a,view.fogRange.xy);
        color=view.options.z!=0 ? color*transmittance : lerp(view.fogColor.rgb,color,transmittance);
    }
    return float4(color*alpha,alpha);
}
[shader("fragment")]
float4 fragmentMain(Varying i):SV_Target { return ShadeParticle(i,false); }
[shader("fragment")]
float4 ribbonFragment(Varying i):SV_Target { return ShadeParticle(i,true); }

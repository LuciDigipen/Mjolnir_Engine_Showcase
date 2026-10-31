struct Particle { float4 positionAge,velocityLifetime,sizeRotation,color; uint4 identity; };
struct Counts { uint vertices,alive,firstVertex,firstInstance,spawned,dropped,generation,reserved; };
struct Args { uint vertices,alive,firstVertex,firstInstance; };
[[vk::binding(3)]] StructuredBuffer<Counts> counts;
[[vk::binding(5)]] StructuredBuffer<Particle> particles;
[[vk::binding(9)]] RWStructuredBuffer<uint2> nodes;
[[vk::binding(11)]] RWStructuredBuffer<Args> arguments;
struct SortView { float4x4 viewProj; float4 anchor; uint4 range,step; float4 limits; };
[[vk::push_constant]] SortView view;
bool Greater(uint2 a,uint2 b) { return a.x>b.x || (a.x==b.x && a.y<b.y); }
groupshared uint2 localNodes[2048];
uint2 VisibleNode(uint i,uint argument)
{
    uint2 node=uint2(0,0xffffffff);
    if(i<min(counts[view.range.w].alive,view.range.y))
    {
        Particle p=particles[view.range.x+i];
        float3 center=view.anchor.xyz+p.positionAge.xyz;
        float radius=abs(p.sizeRotation.x)*1.415+length(p.velocityLifetime.xyz)*view.limits.w;
        float4 clip=mul(view.viewProj,float4(center,1));
        float4 margin=float4(length(view.viewProj[0].xyz),length(view.viewProj[1].xyz),
            length(view.viewProj[2].xyz),length(view.viewProj[3].xyz))*radius;
        bool visible=all(isfinite(center)) && isfinite(radius) && all(isfinite(clip)) &&
            length(center)<=view.limits.x+radius && clip.w+margin.w>0 &&
            abs(clip.x)<=clip.w+margin.x+margin.w && abs(clip.y)<=clip.w+margin.y+margin.w &&
            clip.z>=-margin.z && clip.z<=clip.w+margin.z+margin.w;
        if(visible)
        {
            float key=view.limits.y==0 ? length(center)+1 : float(view.range.y-i);
            node=uint2(asuint(key),view.range.x+i);
            uint ignored;
            InterlockedAdd(arguments[argument].alive,1,ignored);
        }
    }
    return node;
}
[shader("compute")]
[numthreads(128,1,1)]
void computeMain(uint3 id:SV_DispatchThreadID,uint3 thread:SV_GroupThreadID)
{
    uint i=id.x;
    uint base=view.step.x+view.range.x*2;
    uint argument=view.step.y+view.range.w;
    if(view.limits.z==2)
    {
        if(thread.x==0) { arguments[argument].vertices=6;arguments[argument].firstVertex=0;arguments[argument].firstInstance=0; }
        for(uint index=thread.x;index<view.range.z;index+=128) localNodes[index]=VisibleNode(index,argument);
        GroupMemoryBarrierWithGroupSync();
        for(uint stage=2;stage<=view.range.z;stage*=2)
            for(uint pass=stage/2;pass;pass/=2)
            {
                for(uint index=thread.x;index<view.range.z;index+=128)
                {
                    uint partner=index^pass;
                    if(partner>index && partner<view.range.z)
                    {
                        uint2 a=localNodes[index],b=localNodes[partner];
                        bool descending=(index&stage)==0;
                        if(descending ? Greater(b,a) : Greater(a,b)) { localNodes[index]=b;localNodes[partner]=a; }
                    }
                }
                GroupMemoryBarrierWithGroupSync();
            }
        for(uint index=thread.x;index<view.range.z;index+=128) nodes[base+index]=localNodes[index];
        return;
    }
    if(i>=view.range.z) return;
    if(view.limits.z!=0)
    {
        if(i==0) { arguments[argument].vertices=6;arguments[argument].firstVertex=0;arguments[argument].firstInstance=0; }
        nodes[base+i]=VisibleNode(i,argument);
        return;
    }
    uint j=i^view.step.w;
    if(j<=i || j>=view.range.z) return;
    uint2 a=nodes[base+i],b=nodes[base+j];
    bool descending=(i&view.step.z)==0;
    if(descending ? Greater(b,a) : Greater(a,b)) { nodes[base+i]=b;nodes[base+j]=a; }
}

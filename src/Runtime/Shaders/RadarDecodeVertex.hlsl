struct PS_INPUT
{
    float4 position : SV_POSITION;
};

PS_INPUT main(uint vertex_id : SV_VertexID)
{
    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    PS_INPUT output;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

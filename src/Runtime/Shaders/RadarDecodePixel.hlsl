Texture2D<float4> source_texture : register(t0);

float4 main(float4 position : SV_POSITION) : SV_Target
{
    return source_texture.Load(int3(int2(position.xy), 0));
}

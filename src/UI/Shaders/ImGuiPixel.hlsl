// Dear ImGui DX11 backend shader, kept equivalent to imgui_impl_dx11.cpp.
struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv : TEXCOORD0;
};

sampler sampler0;
Texture2D texture0;

float4 main(PS_INPUT input) : SV_Target
{
    return input.col * texture0.Sample(sampler0, input.uv);
}

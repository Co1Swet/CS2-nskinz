#include "Runtime/RadarAlphaFix.hpp"

#include "Runtime/WorkshopMaps.hpp"
#include "Runtime/Shaders/RadarDecodePixel.generated.hpp"
#include "Runtime/Shaders/RadarDecodeVertex.generated.hpp"

#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace
{
using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kDataFourCc = 0x41544144u;
constexpr std::uint8_t kFormatDxt1 = 1;
constexpr std::uint8_t kFormatDxt5 = 2;
constexpr std::uint8_t kFormatRgba8888 = 4;
constexpr std::uint8_t kFormatJpegRgba8888 = 15;
constexpr std::uint8_t kFormatPngRgba8888 = 16;
constexpr std::uint8_t kFormatJpegDxt5 = 17;
constexpr std::uint8_t kFormatPngDxt5 = 18;
constexpr std::uint8_t kFormatBc7 = 20;
constexpr std::uint8_t kFormatBgra8888 = 28;

struct ParsedVtex
{
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t depth = 0;
    std::uint16_t flags = 0;
    std::uint8_t format = 0;
    std::uint8_t mip_levels = 0;
    std::size_t payload_offset = 0;
};

template <typename T>
bool ReadLittleEndian(const std::vector<std::uint8_t>& bytes, const std::size_t offset, T& value)
{
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
    {
        return false;
    }
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return true;
}

bool ParseVtex(
    const std::vector<std::uint8_t>& bytes,
    ParsedVtex& texture,
    std::wstring& diagnostic)
{
    std::uint16_t header_version = 0;
    std::uint32_t block_offset = 0;
    std::uint32_t block_count = 0;
    if (!ReadLittleEndian(bytes, 4, header_version) || header_version != 12 ||
        !ReadLittleEndian(bytes, 8, block_offset) || !ReadLittleEndian(bytes, 12, block_count) ||
        block_count == 0 || block_count > 64)
    {
        diagnostic = L"The selected radar texture has an invalid Source 2 resource header.";
        return false;
    }
    const auto table_offset = static_cast<std::size_t>(8u + block_offset);
    if (table_offset > bytes.size() || bytes.size() - table_offset < block_count * 12ull)
    {
        diagnostic = L"The selected radar texture has a truncated resource block table.";
        return false;
    }

    std::size_t data_offset = 0;
    std::uint32_t data_size = 0;
    for (std::uint32_t index = 0; index < block_count; ++index)
    {
        const auto entry = table_offset + static_cast<std::size_t>(index) * 12u;
        std::uint32_t type = 0;
        std::uint32_t relative = 0;
        std::uint32_t size = 0;
        if (!ReadLittleEndian(bytes, entry, type) || !ReadLittleEndian(bytes, entry + 4, relative) ||
            !ReadLittleEndian(bytes, entry + 8, size))
        {
            diagnostic = L"The selected radar texture has an unreadable resource block.";
            return false;
        }
        if (type == kDataFourCc)
        {
            data_offset = entry + 4u + relative;
            data_size = size;
            break;
        }
    }
    if (data_offset == 0 || data_size < 40 || data_offset > bytes.size() ||
        bytes.size() - data_offset < data_size)
    {
        diagnostic = L"The selected radar texture has no valid VTEX DATA block.";
        return false;
    }

    std::uint16_t version = 0;
    if (!ReadLittleEndian(bytes, data_offset, version) || version != 1 ||
        !ReadLittleEndian(bytes, data_offset + 2, texture.flags) ||
        !ReadLittleEndian(bytes, data_offset + 20, texture.width) ||
        !ReadLittleEndian(bytes, data_offset + 22, texture.height) ||
        !ReadLittleEndian(bytes, data_offset + 24, texture.depth))
    {
        diagnostic = L"The selected radar texture uses an unsupported VTEX header.";
        return false;
    }
    texture.format = bytes[data_offset + 26];
    texture.mip_levels = bytes[data_offset + 27];
    texture.payload_offset = data_offset + data_size;
    if (texture.width == 0 || texture.height == 0 || texture.width > 4096 || texture.height > 4096 ||
        texture.depth != 1 || texture.mip_levels == 0 || texture.mip_levels > 16 ||
        texture.payload_offset > bytes.size())
    {
        diagnostic = L"The selected radar texture dimensions, depth, or mip count are unsupported.";
        return false;
    }
    return true;
}

std::size_t MipDimension(const std::size_t value, const std::uint8_t mip)
{
    return std::max<std::size_t>(value >> mip, 1);
}

std::size_t MipByteSize(
    const std::uint8_t format,
    const std::size_t width,
    const std::size_t height)
{
    if (format == kFormatDxt1)
    {
        return ((width + 3) / 4) * ((height + 3) / 4) * 8;
    }
    if (format == kFormatDxt5 || format == kFormatBc7)
    {
        return ((width + 3) / 4) * ((height + 3) / 4) * 16;
    }
    if (format == kFormatRgba8888 || format == kFormatBgra8888)
    {
        return width * height * 4;
    }
    return 0;
}

bool ResolveTopMip(
    const ParsedVtex& texture,
    const std::vector<std::uint8_t>& bytes,
    const std::uint8_t*& data,
    std::size_t& size,
    std::wstring& diagnostic)
{
    auto offset = texture.payload_offset;
    for (int mip = static_cast<int>(texture.mip_levels) - 1; mip > 0; --mip)
    {
        const auto mip_size = MipByteSize(
            texture.format,
            MipDimension(texture.width, static_cast<std::uint8_t>(mip)),
            MipDimension(texture.height, static_cast<std::uint8_t>(mip)));
        if (mip_size == 0 || offset > bytes.size() || bytes.size() - offset < mip_size)
        {
            diagnostic = L"The selected radar texture uses compressed mip storage that is not safe to rewrite.";
            return false;
        }
        offset += mip_size;
    }
    size = MipByteSize(texture.format, texture.width, texture.height);
    if (size == 0 || offset > bytes.size() || bytes.size() - offset < size)
    {
        diagnostic = L"The selected radar texture payload is compressed or uses an unsupported pixel format.";
        return false;
    }
    data = bytes.data() + offset;
    return true;
}

class ComApartment
{
public:
    ComApartment()
    {
        const auto result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        uninitialize_ = result == S_OK || result == S_FALSE;
    }
    ~ComApartment()
    {
        if (uninitialize_)
        {
            CoUninitialize();
        }
    }

private:
    bool uninitialize_ = false;
};

bool CreateWicFactory(ComPtr<IWICImagingFactory>& factory)
{
    return SUCCEEDED(CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)));
}

bool DecodeWithWic(
    const std::uint8_t* bytes,
    const std::size_t byte_count,
    std::uint16_t& width,
    std::uint16_t& height,
    std::vector<std::uint8_t>& rgba,
    std::wstring& diagnostic)
{
    if (bytes == nullptr || byte_count == 0 || byte_count > std::numeric_limits<UINT>::max())
    {
        diagnostic = L"The embedded radar image payload is empty or too large.";
        return false;
    }
    ComApartment apartment;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (!CreateWicFactory(factory) || FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes), static_cast<DWORD>(byte_count))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom)))
    {
        diagnostic = L"Windows Imaging Component could not decode the radar texture payload.";
        return false;
    }
    UINT decoded_width = 0;
    UINT decoded_height = 0;
    if (FAILED(converter->GetSize(&decoded_width, &decoded_height)) || decoded_width == 0 || decoded_height == 0 ||
        decoded_width > 4096 || decoded_height > 4096)
    {
        diagnostic = L"The decoded radar image dimensions are invalid.";
        return false;
    }
    const auto stride = static_cast<std::size_t>(decoded_width) * 4;
    rgba.resize(stride * decoded_height);
    if (FAILED(converter->CopyPixels(
            nullptr,
            static_cast<UINT>(stride),
            static_cast<UINT>(rgba.size()),
            rgba.data())))
    {
        diagnostic = L"Windows Imaging Component could not copy the decoded radar pixels.";
        return false;
    }
    width = static_cast<std::uint16_t>(decoded_width);
    height = static_cast<std::uint16_t>(decoded_height);
    return true;
}

bool DecodeWithD3d11(
    const ParsedVtex& texture,
    const std::uint8_t* data,
    const std::size_t data_size,
    std::vector<std::uint8_t>& rgba,
    std::wstring& diagnostic)
{
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::size_t row_pitch = 0;
    if (texture.format == kFormatDxt1)
    {
        format = DXGI_FORMAT_BC1_UNORM;
        row_pitch = ((texture.width + 3u) / 4u) * 8u;
    }
    else if (texture.format == kFormatDxt5)
    {
        format = DXGI_FORMAT_BC3_UNORM;
        row_pitch = ((texture.width + 3u) / 4u) * 16u;
    }
    else if (texture.format == kFormatBc7)
    {
        format = DXGI_FORMAT_BC7_UNORM;
        row_pitch = ((texture.width + 3u) / 4u) * 16u;
    }
    else
    {
        diagnostic = L"The radar texture pixel format is not supported by the D3D11 decoder.";
        return false;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    constexpr std::array<D3D_FEATURE_LEVEL, 3> feature_levels {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_10_0;
    auto result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_WARP,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        feature_levels.data(),
        static_cast<UINT>(feature_levels.size()),
        D3D11_SDK_VERSION,
        &device,
        &feature_level,
        &context);
    if (result == E_INVALIDARG)
    {
        result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            feature_levels.data() + 1,
            static_cast<UINT>(feature_levels.size() - 1),
            D3D11_SDK_VERSION,
            &device,
            &feature_level,
            &context);
    }
    if (FAILED(result))
    {
        diagnostic = L"A private D3D11 WARP decoder device could not be created for the radar texture.";
        return false;
    }

    D3D11_TEXTURE2D_DESC source_description {};
    source_description.Width = texture.width;
    source_description.Height = texture.height;
    source_description.MipLevels = 1;
    source_description.ArraySize = 1;
    source_description.Format = format;
    source_description.SampleDesc.Count = 1;
    source_description.Usage = D3D11_USAGE_IMMUTABLE;
    source_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial_data {};
    initial_data.pSysMem = data;
    initial_data.SysMemPitch = static_cast<UINT>(row_pitch);
    initial_data.SysMemSlicePitch = static_cast<UINT>(data_size);
    ComPtr<ID3D11Texture2D> source_texture;
    ComPtr<ID3D11ShaderResourceView> source_view;
    if (FAILED(device->CreateTexture2D(&source_description, &initial_data, &source_texture)) ||
        FAILED(device->CreateShaderResourceView(source_texture.Get(), nullptr, &source_view)))
    {
        diagnostic = L"D3D11 could not create the compressed radar texture decoder input.";
        return false;
    }

    D3D11_TEXTURE2D_DESC output_description {};
    output_description.Width = texture.width;
    output_description.Height = texture.height;
    output_description.MipLevels = 1;
    output_description.ArraySize = 1;
    output_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    output_description.SampleDesc.Count = 1;
    output_description.Usage = D3D11_USAGE_DEFAULT;
    output_description.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> output_texture;
    ComPtr<ID3D11RenderTargetView> output_view;
    if (FAILED(device->CreateTexture2D(&output_description, nullptr, &output_texture)) ||
        FAILED(device->CreateRenderTargetView(output_texture.Get(), nullptr, &output_view)))
    {
        diagnostic = L"D3D11 could not create the radar decoder render target.";
        return false;
    }

    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    if (FAILED(device->CreateVertexShader(
            kRadarDecodeVertexShader,
            sizeof(kRadarDecodeVertexShader),
            nullptr,
            &vertex_shader)) ||
        FAILED(device->CreatePixelShader(
            kRadarDecodePixelShader,
            sizeof(kRadarDecodePixelShader),
            nullptr,
            &pixel_shader)))
    {
        diagnostic = L"D3D11 could not create the embedded radar decoder shaders.";
        return false;
    }

    const D3D11_VIEWPORT viewport {
        0.0f,
        0.0f,
        static_cast<float>(texture.width),
        static_cast<float>(texture.height),
        0.0f,
        1.0f,
    };
    auto* render_target = output_view.Get();
    auto* shader_view = source_view.Get();
    context->OMSetRenderTargets(1, &render_target, nullptr);
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex_shader.Get(), nullptr, 0);
    context->PSSetShader(pixel_shader.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &shader_view);
    context->Draw(3, 0);

    D3D11_TEXTURE2D_DESC staging_description = output_description;
    staging_description.Usage = D3D11_USAGE_STAGING;
    staging_description.BindFlags = 0;
    staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging_texture;
    if (FAILED(device->CreateTexture2D(&staging_description, nullptr, &staging_texture)))
    {
        diagnostic = L"D3D11 could not create the radar decoder readback texture.";
        return false;
    }
    context->CopyResource(staging_texture.Get(), output_texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(staging_texture.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    {
        diagnostic = L"D3D11 could not read back the decoded radar texture.";
        return false;
    }
    const auto destination_pitch = static_cast<std::size_t>(texture.width) * 4;
    rgba.resize(destination_pitch * texture.height);
    for (std::size_t row = 0; row < texture.height; ++row)
    {
        std::memcpy(
            rgba.data() + row * destination_pitch,
            static_cast<const std::uint8_t*>(mapped.pData) + row * mapped.RowPitch,
            destination_pitch);
    }
    context->Unmap(staging_texture.Get(), 0);
    return true;
}

bool DecodeVtexPixels(
    const ParsedVtex& texture,
    const std::vector<std::uint8_t>& source,
    std::uint16_t& width,
    std::uint16_t& height,
    std::vector<std::uint8_t>& rgba,
    std::wstring& diagnostic)
{
    width = texture.width;
    height = texture.height;
    if (texture.format == kFormatPngRgba8888 || texture.format == kFormatPngDxt5 ||
        texture.format == kFormatJpegRgba8888 || texture.format == kFormatJpegDxt5)
    {
        if (texture.payload_offset >= source.size())
        {
            diagnostic = L"The raw radar image payload is missing.";
            return false;
        }
        return DecodeWithWic(
            source.data() + texture.payload_offset,
            source.size() - texture.payload_offset,
            width,
            height,
            rgba,
            diagnostic);
    }

    const std::uint8_t* top_mip = nullptr;
    std::size_t top_mip_size = 0;
    if (!ResolveTopMip(texture, source, top_mip, top_mip_size, diagnostic))
    {
        return false;
    }
    if (texture.format == kFormatRgba8888 || texture.format == kFormatBgra8888)
    {
        rgba.assign(top_mip, top_mip + top_mip_size);
        if (texture.format == kFormatBgra8888)
        {
            for (std::size_t index = 0; index + 3 < rgba.size(); index += 4)
            {
                std::swap(rgba[index], rgba[index + 2]);
            }
        }
        return true;
    }
    return DecodeWithD3d11(texture, top_mip, top_mip_size, rgba, diagnostic);
}

bool ApplyRadarForegroundMask(
    std::vector<std::uint8_t>& rgba,
    const std::uint16_t width,
    const std::uint16_t height,
    std::size_t& visible_pixels,
    std::size_t& transparent_pixels,
    std::size_t& softened_edge_pixels,
    std::size_t& recovered_detail_pixels,
    int& applied_black_tolerance,
    int& detected_neutral_mode,
    std::size_t& detected_neutral_pixels,
    std::size_t& detected_neutral_mode_pixels,
    bool& changed,
    std::wstring& diagnostic,
    const nskinzcs2::runtime::RadarAlphaMaskSettings& settings)
{
    const auto pixel_count = static_cast<std::size_t>(width) * height;
    if (rgba.size() != pixel_count * 4)
    {
        diagnostic = L"The decoded radar pixel buffer has an invalid size.";
        return false;
    }
    std::size_t already_transparent = 0;
    std::size_t opaque_near_black = 0;
    constexpr int kDefaultBlackTolerance = 12;
    const auto configured_black_tolerance = std::clamp(settings.black_tolerance, 0, 96);
    // Wide dark corridors in water/night conversions can exceed the previous
    // six-pixel cap. Recovery still requires retained map pixels on opposite
    // sides and remains confined to the main map bounds.
    const auto dark_detail_radius = std::clamp(settings.dark_detail_radius, 0, 32);
    const auto background_alpha_percent =
        std::clamp(settings.background_alpha_percent, 0, 100);
    // Some Workshop overviews bake a neutral editor grid one or two values
    // above the configured cutoff (Ancient Night uses an opaque 25/255 grid
    // with the default cutoff at 24). Find the dominant opaque neutral matte
    // value. The user setting is a signed correction around the road-safe
    // default, not a minimum: moving the slider in either direction must
    // change the effective cutoff instead of being hidden by auto-detection.
    std::array<std::size_t, 65> neutral_dark_histogram {};
    std::size_t neutral_dark_pixels = 0;
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel)
    {
        const auto offset = pixel * 4;
        const auto red = rgba[offset];
        const auto green = rgba[offset + 1];
        const auto blue = rgba[offset + 2];
        const auto alpha = rgba[offset + 3];
        const auto maximum = std::max({red, green, blue});
        const auto minimum = std::min({red, green, blue});
        const auto chroma = static_cast<int>(maximum) - minimum;
        if (alpha < 32)
        {
            ++already_transparent;
        }
        if (alpha > 240 && maximum <= 64 && chroma <= 4)
        {
            ++neutral_dark_histogram[maximum];
            ++neutral_dark_pixels;
        }
    }
    int dominant_neutral_value = 0;
    std::size_t dominant_neutral_count = 0;
    for (int value = 0; value <= 64; ++value)
    {
        if (neutral_dark_histogram[static_cast<std::size_t>(value)] > dominant_neutral_count)
        {
            dominant_neutral_value = value;
            dominant_neutral_count = neutral_dark_histogram[static_cast<std::size_t>(value)];
        }
    }
    // A fully black field can be the dominant value while the baked grid is
    // a second, brighter neutral peak just above the configured cutoff. Look
    // for that peak only when the black field itself occupies a large part of
    // the image; this avoids treating ordinary dark map shading as a matte.
    auto detected_matte_value = dominant_neutral_value;
    auto detected_matte_count = dominant_neutral_count;
    if (dominant_neutral_count >= pixel_count / 4 &&
        dominant_neutral_value <= configured_black_tolerance &&
        configured_black_tolerance < 40)
    {
        int secondary_value = 0;
        std::size_t secondary_count = 0;
        for (int value = configured_black_tolerance + 1; value <= 40; ++value)
        {
            const auto count = neutral_dark_histogram[static_cast<std::size_t>(value)];
            if (count > secondary_count)
            {
                secondary_value = value;
                secondary_count = count;
            }
        }
        if (secondary_count >= std::max<std::size_t>(pixel_count / 1000, 64))
        {
            detected_matte_value = secondary_value;
            detected_matte_count = secondary_count;
        }
    }
    detected_neutral_mode = detected_matte_value;
    detected_neutral_pixels = neutral_dark_pixels;
    detected_neutral_mode_pixels = detected_matte_count;
    auto black_tolerance = configured_black_tolerance;
    if (neutral_dark_pixels >= pixel_count / 20 &&
        detected_matte_value > 0 &&
        detected_matte_count >= std::max<std::size_t>(pixel_count / 2000, 64))
    {
        const auto user_correction = configured_black_tolerance - kDefaultBlackTolerance;
        black_tolerance = std::clamp(detected_matte_value + 2 + user_correction, 0, 96);
    }
    applied_black_tolerance = black_tolerance;
    const auto coloured_detail_floor = std::max(1, black_tolerance * 2 / 3);
    const auto coloured_detail_chroma = std::clamp(black_tolerance * 3 / 8, 3, 24);
    std::vector<std::uint8_t> map_pixel(pixel_count, 0);
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel)
    {
        const auto offset = pixel * 4;
        const auto red = rgba[offset];
        const auto green = rgba[offset + 1];
        const auto blue = rgba[offset + 2];
        const auto alpha = rgba[offset + 3];
        const auto maximum = std::max({red, green, blue});
        const auto minimum = std::min({red, green, blue});
        const auto chroma = static_cast<int>(maximum) - minimum;
        if (alpha > 240 && maximum < std::max(48, black_tolerance + 24) && chroma < 6)
        {
            ++opaque_near_black;
        }
        // Workshop overviews commonly bake the editor's opaque black grid
        // into the radar image. CS2's cl_hud_radar_background_alpha only
        // controls the separate Panorama background layer, so that baked
        // field must become transparent before the native slider can behave
        // like it does for Valve maps. Workshop authors use different matte
        // brightness levels, so the cutoff is configurable. Saturated detail
        // gets a lower floor to retain coloured labels and floor accents.
        if (alpha > 8)
        {
            map_pixel[pixel] = maximum > black_tolerance ||
                (maximum > coloured_detail_floor && chroma > coloured_detail_chroma);
        }
    }
    if (opaque_near_black < pixel_count / 20 ||
        (already_transparent > pixel_count / 10 && opaque_near_black < pixel_count / 3))
    {
        visible_pixels = pixel_count - already_transparent;
        transparent_pixels = already_transparent;
        softened_edge_pixels = 0;
        recovered_detail_pixels = 0;
        changed = false;
        return true;
    }

    struct ComponentInfo
    {
        std::size_t size = 0;
        int minimum_x = 0;
        int minimum_y = 0;
        int maximum_x = 0;
        int maximum_y = 0;
        int maximum_colour_value = 0;
        int maximum_chroma = 0;
    };

    constexpr auto kUnassignedComponent = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> component_by_pixel(pixel_count, kUnassignedComponent);
    std::vector<ComponentInfo> components;
    std::vector<std::size_t> queue;
    queue.reserve(pixel_count / 4);
    std::uint32_t largest_component = kUnassignedComponent;
    for (std::size_t start = 0; start < pixel_count; ++start)
    {
        if (!map_pixel[start] || component_by_pixel[start] != kUnassignedComponent)
        {
            continue;
        }
        const auto component_id = static_cast<std::uint32_t>(components.size());
        queue.clear();
        queue.push_back(start);
        component_by_pixel[start] = component_id;
        ComponentInfo component;
        component.minimum_x = component.maximum_x = static_cast<int>(start % width);
        component.minimum_y = component.maximum_y = static_cast<int>(start / width);
        for (std::size_t cursor = 0; cursor < queue.size(); ++cursor)
        {
            const auto current = queue[cursor];
            const auto x = static_cast<int>(current % width);
            const auto y = static_cast<int>(current / width);
            ++component.size;
            component.minimum_x = std::min(component.minimum_x, x);
            component.minimum_y = std::min(component.minimum_y, y);
            component.maximum_x = std::max(component.maximum_x, x);
            component.maximum_y = std::max(component.maximum_y, y);
            const auto current_offset = current * 4;
            component.maximum_colour_value = std::max(
                component.maximum_colour_value,
                static_cast<int>(std::max({
                    rgba[current_offset], rgba[current_offset + 1], rgba[current_offset + 2]})));
            component.maximum_chroma = std::max(
                component.maximum_chroma,
                static_cast<int>(std::max({
                    rgba[current_offset], rgba[current_offset + 1], rgba[current_offset + 2]})) -
                    static_cast<int>(std::min({
                        rgba[current_offset], rgba[current_offset + 1], rgba[current_offset + 2]})));
            for (int delta_y = -1; delta_y <= 1; ++delta_y)
            {
                for (int delta_x = -1; delta_x <= 1; ++delta_x)
                {
                    if ((delta_x == 0 && delta_y == 0) || x + delta_x < 0 || y + delta_y < 0 ||
                        x + delta_x >= width || y + delta_y >= height)
                    {
                        continue;
                    }
                    const auto neighbour = static_cast<std::size_t>(y + delta_y) * width + (x + delta_x);
                    if (map_pixel[neighbour] && component_by_pixel[neighbour] == kUnassignedComponent)
                    {
                        component_by_pixel[neighbour] = component_id;
                        queue.push_back(neighbour);
                    }
                }
            }
        }
        components.push_back(component);
        if (largest_component == kUnassignedComponent ||
            component.size > components[largest_component].size)
        {
            largest_component = component_id;
        }
    }
    if (largest_component == kUnassignedComponent ||
        components[largest_component].size < std::max<std::size_t>(pixel_count / 200, 32))
    {
        diagnostic = L"The radar foreground could not be separated safely from its opaque background.";
        return false;
    }

    // A playable room or decoration can be separated from the main floor plan
    // by a fully black source pixel. Retain coloured components inside the
    // main map bounds even when they are small. The discarded coordinate-grid
    // fragments are both outside those bounds and much darker.
    const auto& main_component = components[largest_component];
    std::vector<std::uint8_t> retained_component(components.size(), 0);
    for (std::size_t index = 0; index < components.size(); ++index)
    {
        const auto& component = components[index];
        const bool inside_main_bounds =
            component.minimum_x >= main_component.minimum_x &&
            component.maximum_x <= main_component.maximum_x &&
            component.minimum_y >= main_component.minimum_y &&
            component.maximum_y <= main_component.maximum_y;
        const bool overlaps_main_bounds =
            component.maximum_x >= main_component.minimum_x &&
            component.minimum_x <= main_component.maximum_x &&
            component.maximum_y >= main_component.minimum_y &&
            component.minimum_y <= main_component.maximum_y;
        const bool coloured_component =
            component.maximum_chroma > coloured_detail_chroma;
        const bool substantial_neutral_component =
            component.size >= 32 &&
            component.maximum_colour_value > black_tolerance + 4;
        retained_component[index] =
            index == largest_component ||
            (inside_main_bounds && component.maximum_colour_value > 32 &&
                (coloured_component || substantial_neutral_component)) ||
            (overlaps_main_bounds && component.size >= 16 &&
                component.maximum_colour_value > black_tolerance &&
                (coloured_component || component.size >= 64));
    }

    std::vector<std::uint8_t> foreground(pixel_count, 0);
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel)
    {
        if (!map_pixel[pixel] ||
            component_by_pixel[pixel] == kUnassignedComponent ||
            !retained_component[component_by_pixel[pixel]])
        {
            continue;
        }
        foreground[pixel] = 1;
    }

    // Recover only dark runs that are bounded by retained map pixels on
    // opposite sides. A simple dilation also grows the outside matte and is
    // the reason earlier builds left a black halo. Opposite-side bridging
    // restores thin neutral corridors / catwalks (common in Nuke overviews)
    // while leaving one-sided editor grid lines outside the map transparent.
    recovered_detail_pixels = 0;
    std::vector<std::uint8_t> recovered_detail(pixel_count, 0);
    if (dark_detail_radius > 0)
    {
        const auto has_foreground_in_direction = [&foreground, width, height](
            const int x,
            const int y,
            const int dx,
            const int dy,
            const int radius)
        {
            for (int distance = 1; distance <= radius; ++distance)
            {
                const auto target_x = x + dx * distance;
                const auto target_y = y + dy * distance;
                if (target_x < 0 || target_y < 0 || target_x >= width || target_y >= height)
                {
                    return false;
                }
                if (foreground[static_cast<std::size_t>(target_y) * width + target_x])
                {
                    return true;
                }
            }
            return false;
        };
        for (std::size_t pixel = 0; pixel < pixel_count; ++pixel)
        {
            if (foreground[pixel])
            {
                continue;
            }
            const auto x = static_cast<int>(pixel % width);
            const auto y = static_cast<int>(pixel / width);
            if (x < main_component.minimum_x || x > main_component.maximum_x ||
                y < main_component.minimum_y || y > main_component.maximum_y)
            {
                continue;
            }
            const auto offset = pixel * 4;
            const auto maximum = static_cast<int>(std::max({
                rgba[offset], rgba[offset + 1], rgba[offset + 2]}));
            const auto minimum = static_cast<int>(std::min({
                rgba[offset], rgba[offset + 1], rgba[offset + 2]}));
            if (rgba[offset + 3] <= 8 || maximum > black_tolerance ||
                maximum - minimum > coloured_detail_chroma)
            {
                continue;
            }
            const auto bounded_axis_count = [&](const int radius)
            {
                int count = 0;
                count += has_foreground_in_direction(x, y, -1, 0, radius) &&
                    has_foreground_in_direction(x, y, 1, 0, radius);
                count += has_foreground_in_direction(x, y, 0, -1, radius) &&
                    has_foreground_in_direction(x, y, 0, 1, radius);
                count += has_foreground_in_direction(x, y, -1, -1, radius) &&
                    has_foreground_in_direction(x, y, 1, 1, radius);
                count += has_foreground_in_direction(x, y, 1, -1, radius) &&
                    has_foreground_in_direction(x, y, -1, 1, radius);
                return count;
            };
            const auto conservative_radius = std::min(dark_detail_radius, 4);
            const bool short_bounded_gap = bounded_axis_count(conservative_radius) > 0;
            const bool enclosed_wide_gap =
                dark_detail_radius > conservative_radius && bounded_axis_count(dark_detail_radius) >= 1;
            if (short_bounded_gap || enclosed_wide_gap)
            {
                recovered_detail[pixel] = 1;
            }
        }
        for (std::size_t pixel = 0; pixel < pixel_count; ++pixel)
        {
            if (recovered_detail[pixel])
            {
                foreground[pixel] = 1;
            }
        }
    }

    // The baked grid has a one-pixel near-black antialias fringe at many map
    // boundaries. It lies just outside the solid mask; deleting it creates
    // visible pinholes. Mark one adjacent band for black-matte recovery:
    // coverage becomes alpha and RGB comes from the adjacent floor colour.
    std::vector<std::uint8_t> soften_edge(pixel_count, 0);
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel)
    {
        if (foreground[pixel])
        {
            continue;
        }
        const auto offset = pixel * 4;
        const auto maximum = std::max({rgba[offset], rgba[offset + 1], rgba[offset + 2]});
        if (rgba[offset + 3] <= 8 || maximum < 4 || maximum > black_tolerance)
        {
            continue;
        }
        const auto x = static_cast<int>(pixel % width);
        const auto y = static_cast<int>(pixel / width);
        for (int delta_y = -1; delta_y <= 1 && !soften_edge[pixel]; ++delta_y)
        {
            for (int delta_x = -1; delta_x <= 1; ++delta_x)
            {
                if ((delta_x == 0 && delta_y == 0) || x + delta_x < 0 || y + delta_y < 0 ||
                    x + delta_x >= width || y + delta_y >= height)
                {
                    continue;
                }
                const auto neighbour = static_cast<std::size_t>(y + delta_y) * width + (x + delta_x);
                if (foreground[neighbour])
                {
                    soften_edge[pixel] = 1;
                    break;
                }
            }
        }
    }
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel)
    {
        if (soften_edge[pixel])
        {
            foreground[pixel] = 1;
        }
    }

    // A recovered gap was classified as background in the source image. Do
    // not retain its original near-black RGB: that recreated opaque black
    // crumbs in Inferno Water even though the surrounding matte had become
    // transparent. Preserve the recovered coverage, but borrow RGB from the
    // nearest trusted foreground pixel. If no trusted neighbour exists, the
    // candidate is not a safe road/detail recovery and remains background.
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel)
    {
        if (!recovered_detail[pixel])
        {
            continue;
        }
        const auto x = static_cast<int>(pixel % width);
        const auto y = static_cast<int>(pixel / width);
        std::size_t reference_pixel = pixel;
        bool found_reference = false;
        for (int radius = 1; radius <= std::max(dark_detail_radius, 4) && !found_reference; ++radius)
        {
            int best_maximum = -1;
            for (int delta_y = -radius; delta_y <= radius; ++delta_y)
            {
                for (int delta_x = -radius; delta_x <= radius; ++delta_x)
                {
                    if (std::max(std::abs(delta_x), std::abs(delta_y)) != radius ||
                        x + delta_x < 0 || y + delta_y < 0 ||
                        x + delta_x >= width || y + delta_y >= height)
                    {
                        continue;
                    }
                    const auto candidate =
                        static_cast<std::size_t>(y + delta_y) * width + (x + delta_x);
                    if (!foreground[candidate] || recovered_detail[candidate] || soften_edge[candidate] ||
                        rgba[candidate * 4 + 3] <= 8)
                    {
                        continue;
                    }
                    const auto candidate_offset = candidate * 4;
                    const auto candidate_maximum = static_cast<int>(std::max({
                        rgba[candidate_offset],
                        rgba[candidate_offset + 1],
                        rgba[candidate_offset + 2]}));
                    if (candidate_maximum > best_maximum)
                    {
                        best_maximum = candidate_maximum;
                        reference_pixel = candidate;
                        found_reference = true;
                    }
                }
            }
        }
        if (!found_reference)
        {
            foreground[pixel] = 0;
            recovered_detail[pixel] = 0;
            continue;
        }
        const auto offset = pixel * 4;
        const auto reference_offset = reference_pixel * 4;
        if (rgba[offset] != rgba[reference_offset] ||
            rgba[offset + 1] != rgba[reference_offset + 1] ||
            rgba[offset + 2] != rgba[reference_offset + 2])
        {
            rgba[offset] = rgba[reference_offset];
            rgba[offset + 1] = rgba[reference_offset + 1];
            rgba[offset + 2] = rgba[reference_offset + 2];
            changed = true;
        }
        ++recovered_detail_pixels;
    }
    transparent_pixels = 0;
    softened_edge_pixels = 0;
    changed = false;
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel)
    {
        auto& alpha = rgba[pixel * 4 + 3];
        if (!foreground[pixel])
        {
            const auto target_alpha = static_cast<std::uint8_t>(
                (static_cast<unsigned int>(alpha) *
                    static_cast<unsigned int>(background_alpha_percent) + 50u) / 100u);
            if (alpha != target_alpha)
            {
                alpha = target_alpha;
                changed = true;
            }
        }
        else if (soften_edge[pixel])
        {
            const auto x = static_cast<int>(pixel % width);
            const auto y = static_cast<int>(pixel / width);
            std::size_t reference_pixel = pixel;
            bool found_reference = false;
            for (int radius = 1; radius <= 4 && !found_reference; ++radius)
            {
                int best_maximum = -1;
                for (int delta_y = -radius; delta_y <= radius; ++delta_y)
                {
                    for (int delta_x = -radius; delta_x <= radius; ++delta_x)
                    {
                        if (std::max(std::abs(delta_x), std::abs(delta_y)) != radius ||
                            x + delta_x < 0 || y + delta_y < 0 ||
                            x + delta_x >= width || y + delta_y >= height)
                        {
                            continue;
                        }
                        const auto candidate = static_cast<std::size_t>(y + delta_y) * width + (x + delta_x);
                        if (!foreground[candidate] || soften_edge[candidate])
                        {
                            continue;
                        }
                        const auto candidate_offset = candidate * 4;
                        const auto candidate_maximum = static_cast<int>(std::max({
                            rgba[candidate_offset], rgba[candidate_offset + 1], rgba[candidate_offset + 2]}));
                        if (candidate_maximum > std::max(32, black_tolerance + 8) &&
                            candidate_maximum > best_maximum)
                        {
                            best_maximum = candidate_maximum;
                            reference_pixel = candidate;
                            found_reference = true;
                        }
                    }
                }
            }
            if (found_reference)
            {
                const auto offset = pixel * 4;
                const auto reference_offset = reference_pixel * 4;
                const auto source_maximum = static_cast<unsigned int>(std::max({
                    rgba[offset], rgba[offset + 1], rgba[offset + 2]}));
                const auto reference_maximum = static_cast<unsigned int>(std::max({
                    rgba[reference_offset], rgba[reference_offset + 1], rgba[reference_offset + 2]}));
                const auto original_alpha = static_cast<unsigned int>(alpha);
                const auto recovered_alpha = static_cast<std::uint8_t>(std::clamp(
                    (original_alpha * source_maximum + reference_maximum / 2) / reference_maximum,
                    1u,
                    std::min(original_alpha, 220u)));
                rgba[offset] = rgba[reference_offset];
                rgba[offset + 1] = rgba[reference_offset + 1];
                rgba[offset + 2] = rgba[reference_offset + 2];
                alpha = recovered_alpha;
                ++softened_edge_pixels;
                changed = true;
            }
            else
            {
                alpha = 0;
                foreground[pixel] = 0;
                changed = true;
            }
        }
        if (alpha == 0)
        {
            ++transparent_pixels;
        }
    }
    visible_pixels = static_cast<std::size_t>(std::count(
        foreground.begin(), foreground.end(), static_cast<std::uint8_t>(1)));
    if (visible_pixels < pixel_count / 100 || visible_pixels > pixel_count * 9 / 10)
    {
        diagnostic = L"The generated radar foreground mask failed its coverage safety check.";
        return false;
    }

    // Transparent black RGB bleeds into an otherwise correct alpha edge when
    // ResourceCompiler builds mipmaps and compresses the image as BC7. Seed a
    // nearest-colour expansion from every visible pixel while leaving alpha at
    // zero. This changes no visible pixel, but filtered boundary samples no
    // longer acquire a black saw-tooth halo in Panorama.
    std::vector<std::uint8_t> colour_assigned(pixel_count, 0);
    std::vector<std::size_t> colour_queue;
    colour_queue.reserve(pixel_count);
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel)
    {
        if (rgba[pixel * 4 + 3] != 0)
        {
            colour_assigned[pixel] = 1;
            colour_queue.push_back(pixel);
        }
    }
    constexpr std::array<std::pair<int, int>, 4> kCardinalNeighbours {{
        {-1, 0}, {1, 0}, {0, -1}, {0, 1},
    }};
    for (std::size_t cursor = 0; cursor < colour_queue.size(); ++cursor)
    {
        const auto source_pixel = colour_queue[cursor];
        const auto source_x = static_cast<int>(source_pixel % width);
        const auto source_y = static_cast<int>(source_pixel / width);
        for (const auto [delta_x, delta_y] : kCardinalNeighbours)
        {
            const auto target_x = source_x + delta_x;
            const auto target_y = source_y + delta_y;
            if (target_x < 0 || target_y < 0 || target_x >= width || target_y >= height)
            {
                continue;
            }
            const auto target_pixel = static_cast<std::size_t>(target_y) * width + target_x;
            if (colour_assigned[target_pixel])
            {
                continue;
            }
            const auto source_offset = source_pixel * 4;
            const auto target_offset = target_pixel * 4;
            rgba[target_offset] = rgba[source_offset];
            rgba[target_offset + 1] = rgba[source_offset + 1];
            rgba[target_offset + 2] = rgba[source_offset + 2];
            colour_assigned[target_pixel] = 1;
            colour_queue.push_back(target_pixel);
        }
    }
    return true;
}

bool EncodePng(
    const std::vector<std::uint8_t>& rgba,
    const std::uint16_t width,
    const std::uint16_t height,
    std::vector<std::uint8_t>& png,
    std::wstring& diagnostic)
{
    ComApartment apartment;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if (!CreateWicFactory(factory) || FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, &properties)) || FAILED(frame->Initialize(properties.Get())) ||
        FAILED(frame->SetSize(width, height)))
    {
        diagnostic = L"Windows Imaging Component could not initialize the radar PNG encoder.";
        return false;
    }
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&pixel_format)) || pixel_format != GUID_WICPixelFormat32bppBGRA)
    {
        diagnostic = L"Windows Imaging Component rejected the BGRA radar pixel format.";
        return false;
    }
    auto bgra = rgba;
    for (std::size_t offset = 0; offset + 3 < bgra.size(); offset += 4)
    {
        std::swap(bgra[offset], bgra[offset + 2]);
    }
    const auto stride = static_cast<UINT>(width) * 4u;
    if (FAILED(frame->WritePixels(height, stride, static_cast<UINT>(bgra.size()),
            bgra.data())) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
    {
        diagnostic = L"Windows Imaging Component could not encode the transparent radar PNG.";
        return false;
    }
    STATSTG status {};
    LARGE_INTEGER beginning {};
    if (FAILED(stream->Stat(&status, STATFLAG_NONAME)) || status.cbSize.QuadPart <= 0 ||
        status.cbSize.QuadPart > 64ll * 1024ll * 1024ll || FAILED(stream->Seek(beginning, STREAM_SEEK_SET, nullptr)))
    {
        diagnostic = L"The encoded radar PNG stream has an invalid size.";
        return false;
    }
    png.resize(static_cast<std::size_t>(status.cbSize.QuadPart));
    ULONG read = 0;
    if (FAILED(stream->Read(png.data(), static_cast<ULONG>(png.size()), &read)) || read != png.size())
    {
        diagnostic = L"The encoded radar PNG stream could not be read back.";
        return false;
    }
    return true;
}

std::string BuildContentAddressedRadarVirtualPath(
    const std::string_view request_virtual_path,
    const std::vector<std::uint8_t>& png)
{
    std::uint64_t hash = 14695981039346656037ull;
    const auto mix = [&hash](const std::uint8_t value)
    {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    for (const auto character : request_virtual_path)
    {
        mix(static_cast<std::uint8_t>(character));
    }
    for (const auto byte : png)
    {
        mix(byte);
    }
    std::array<char, 17> hexadecimal {};
    std::snprintf(
        hexadecimal.data(),
        hexadecimal.size(),
        "%016llx",
        static_cast<unsigned long long>(hash));
    return "panorama/images/overheadmaps/nskinz_" + std::string(hexadecimal.data()) +
        "_radar_tga.vtex_c";
}

nskinzcs2::runtime::RadarAlphaMaskSettings ResolveRadarMaskSettings(
    const std::string_view virtual_path,
    const nskinzcs2::runtime::RadarAlphaMaskSettings& settings)
{
    auto resolved = settings;
    // Mirage Water contains legitimate dark road spans that are wider than
    // the generic 12-pixel bridge. They remain bounded by retained floor
    // pixels, so using the algorithm's existing 32-pixel safe maximum restores
    // those roads without dilating the exterior matte. Radius zero remains an
    // explicit opt-out for diagnostics and user control.
    if (resolved.dark_detail_radius > 0 &&
        virtual_path.find("de_mirage_water_radar") != std::string_view::npos)
    {
        resolved.dark_detail_radius = std::max(resolved.dark_detail_radius, 32);
    }
    return resolved;
}

}

nskinzcs2::runtime::RadarAlphaFixOutcome nskinzcs2::runtime::BuildRadarAlphaOverlayFromVtex(
    const std::string_view virtual_path,
    const std::vector<std::uint8_t>& source_vtex,
    RadarAlphaOverlay& output,
    std::wstring& diagnostic,
    const RadarAlphaMaskSettings& settings)
{
    output = {};
    diagnostic.clear();
    ParsedVtex texture;
    if (!ParseVtex(source_vtex, texture, diagnostic))
    {
        return RadarAlphaFixOutcome::Unsupported;
    }
    std::vector<std::uint8_t> rgba;
    std::uint16_t decoded_width = 0;
    std::uint16_t decoded_height = 0;
    if (!DecodeVtexPixels(texture, source_vtex, decoded_width, decoded_height, rgba, diagnostic))
    {
        return RadarAlphaFixOutcome::Unsupported;
    }
    const auto resolved_settings = ResolveRadarMaskSettings(virtual_path, settings);
    bool changed = false;
    if (!ApplyRadarForegroundMask(
            rgba,
            decoded_width,
            decoded_height,
            output.visible_pixels,
            output.transparent_pixels,
            output.softened_edge_pixels,
            output.recovered_detail_pixels,
            output.applied_black_tolerance,
            output.detected_neutral_mode,
            output.detected_neutral_pixels,
            output.detected_neutral_mode_pixels,
            changed,
            diagnostic,
            resolved_settings))
    {
        return RadarAlphaFixOutcome::Unsupported;
    }
    output.request_virtual_path = virtual_path;
    output.width = decoded_width;
    output.height = decoded_height;
    const auto pixel_count = static_cast<std::size_t>(decoded_width) * decoded_height;
    const bool normalize_existing_transparency =
        output.transparent_pixels > pixel_count / 10;
    if (!changed && !normalize_existing_transparency)
    {
        diagnostic = L"The selected radar texture has no opaque black field that needs an alpha overlay.";
        return RadarAlphaFixOutcome::NotNeeded;
    }
    // A decoded Workshop VTEX can contain plenty of zero-alpha pixels while
    // its original resource metadata still makes Panorama / mirv_pov display
    // those pixels over an opaque black field.  Do not treat decoded alpha as
    // proof that the packaged resource renders correctly.  Re-encode and
    // compile already-transparent Workshop radars into the same known-good,
    // cache-isolated native VTEX used by the mask path.  The RGBA pixels are
    // preserved; only the resource representation is normalized.
    std::vector<std::uint8_t> png;
    if (!EncodePng(rgba, decoded_width, decoded_height, png, diagnostic))
    {
        output = {};
        return RadarAlphaFixOutcome::Failed;
    }
    output.virtual_path = BuildContentAddressedRadarVirtualPath(virtual_path, png);
    output.png_bytes = std::move(png);
    output.vtex_bytes.clear();
    diagnostic.clear();
    return RadarAlphaFixOutcome::Generated;
}

nskinzcs2::runtime::RadarAlphaFixOutcome nskinzcs2::runtime::BuildWorkshopRadarAlphaOverlay(
    const std::string_view workshop_id,
    const std::string_view target_map_name,
    RadarAlphaOverlay& output,
    std::wstring& diagnostic,
    const DWORD cs2_process_id,
    const RadarAlphaMaskSettings& settings)
{
    const auto source = ReadWorkshopRadarTexture(
        workshop_id,
        target_map_name,
        cs2_process_id,
        &diagnostic);
    if (!source.has_value())
    {
        output = {};
        return RadarAlphaFixOutcome::Unsupported;
    }
    return BuildRadarAlphaOverlayFromVtex(
        source->virtual_path,
        source->bytes,
        output,
        diagnostic,
        settings);
}

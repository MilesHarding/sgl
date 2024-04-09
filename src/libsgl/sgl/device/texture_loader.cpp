// SPDX-License-Identifier: Apache-2.0

#include "texture_loader.h"

#include "sgl/device/device.h"
#include "sgl/device/command.h"
#include "sgl/device/blit.h"

#include "sgl/core/error.h"
#include "sgl/core/bitmap.h"
#include "sgl/core/timer.h"
#include "sgl/core/thread.h"

#include <map>

namespace sgl {

static constexpr size_t BATCH_SIZE = 32;

/**
 * \brief Determine the texture format given a bitmap.
 *
 * Uses the following option flags to affect the format determination:
 * - \c Options::extend_alpha
 *   RGB bitmap that has no supported format will be determined as RGBA (if a RGBA format exists).
 * - \c Options::load_as_srgb
 *   8-bit RGBA bitmap with sRGB gamma will be determined as \c Format::rgba8_unorm_srgb.
 * - \c Options::load_as_normalized
 *   8/16-bit integer bitmap will be determined as normalized resource format.
 *
 * \param bitmap Bitmap to determine format for.
 * \param options Texture loading options.
 * \return A pair containing the determined format and flag if the bitmap needs to be converted to RGBA to match the format.
 */
inline std::pair<Format, bool> determine_texture_format(const Bitmap* bitmap, const TextureLoader::Options& options)
{
    SGL_ASSERT(bitmap != nullptr);

    using PixelFormat = Bitmap::PixelFormat;
    using ComponentType = Bitmap::ComponentType;

    enum class FormatFlags {
        none = 0,
        normalized = 1,
        srgb = 2,
    };

    auto make_key = [](PixelFormat pixel_format, ComponentType component_type, FormatFlags flags = FormatFlags::none
                    ) constexpr -> uint32_t
    {
        static_assert(Bitmap::PIXEL_FORMAT_COUNT <= 8);
        static_assert(Struct::TYPE_COUNT <= 16);
        uint32_t key = 0;
        key |= (1 << uint32_t(pixel_format));
        key |= (1 << uint32_t(component_type)) << 8;
        key |= uint32_t(flags) << 24;
        return key;
    };

    static const std::map<uint32_t, Format> FORMAT_TABLE{
        // PixelFormat::r
        {make_key(PixelFormat::r, ComponentType::int8), Format::r8_sint},
        {make_key(PixelFormat::r, ComponentType::int8, FormatFlags::normalized), Format::r8_snorm},
        {make_key(PixelFormat::r, ComponentType::int16), Format::r16_sint},
        {make_key(PixelFormat::r, ComponentType::int16, FormatFlags::normalized), Format::r16_snorm},
        {make_key(PixelFormat::r, ComponentType::int32), Format::r32_sint},
        {make_key(PixelFormat::r, ComponentType::uint8), Format::r8_uint},
        {make_key(PixelFormat::r, ComponentType::uint8, FormatFlags::normalized), Format::r8_unorm},
        {make_key(PixelFormat::r, ComponentType::uint16), Format::r16_uint},
        {make_key(PixelFormat::r, ComponentType::uint16, FormatFlags::normalized), Format::r16_unorm},
        {make_key(PixelFormat::r, ComponentType::uint32), Format::r32_uint},
        {make_key(PixelFormat::r, ComponentType::float16), Format::r16_float},
        {make_key(PixelFormat::r, ComponentType::float32), Format::r32_float},
        // PixelFormat::rg,
        {make_key(PixelFormat::rg, ComponentType::int8), Format::rg8_sint},
        {make_key(PixelFormat::rg, ComponentType::int8, FormatFlags::normalized), Format::rg8_snorm},
        {make_key(PixelFormat::rg, ComponentType::int16), Format::rg16_sint},
        {make_key(PixelFormat::rg, ComponentType::int16, FormatFlags::normalized), Format::rg16_snorm},
        {make_key(PixelFormat::rg, ComponentType::int32), Format::rg32_sint},
        {make_key(PixelFormat::rg, ComponentType::uint8), Format::rg8_uint},
        {make_key(PixelFormat::rg, ComponentType::uint8, FormatFlags::normalized), Format::rg8_unorm},
        {make_key(PixelFormat::rg, ComponentType::uint16), Format::rg16_uint},
        {make_key(PixelFormat::rg, ComponentType::uint16, FormatFlags::normalized), Format::rg16_unorm},
        {make_key(PixelFormat::rg, ComponentType::uint32), Format::rg32_uint},
        {make_key(PixelFormat::rg, ComponentType::float16), Format::rg16_float},
        {make_key(PixelFormat::rg, ComponentType::float32), Format::rg32_float},
        // PixelFormat::rgb,
        {make_key(PixelFormat::rgb, ComponentType::int32), Format::rgb32_sint},
        {make_key(PixelFormat::rgb, ComponentType::uint32), Format::rgb32_uint},
        {make_key(PixelFormat::rgb, ComponentType::float32), Format::rgb32_float},
        // PixelFormat::rgba,
        {make_key(PixelFormat::rgba, ComponentType::int8), Format::rgba8_sint},
        {make_key(PixelFormat::rgba, ComponentType::int8, FormatFlags::normalized), Format::rgba8_snorm},
        {make_key(PixelFormat::rgba, ComponentType::int16), Format::rgba16_sint},
        {make_key(PixelFormat::rgba, ComponentType::int16, FormatFlags::normalized), Format::rgba16_snorm},
        {make_key(PixelFormat::rgba, ComponentType::int32), Format::rgba32_sint},
        {make_key(PixelFormat::rgba, ComponentType::uint8), Format::rgba8_uint},
        {make_key(PixelFormat::rgba, ComponentType::uint8, FormatFlags::normalized), Format::rgba8_unorm},
        {make_key(PixelFormat::rgba, ComponentType::uint8, FormatFlags::srgb), Format::rgba8_unorm_srgb},
        {make_key(PixelFormat::rgba, ComponentType::uint16), Format::rgba16_uint},
        {make_key(PixelFormat::rgba, ComponentType::uint16, FormatFlags::normalized), Format::rgba16_unorm},
        {make_key(PixelFormat::rgba, ComponentType::uint32), Format::rgba32_uint},
        {make_key(PixelFormat::rgba, ComponentType::float16), Format::rgba16_float},
        {make_key(PixelFormat::rgba, ComponentType::float32), Format::rgba32_float},
    };

    PixelFormat pixel_format = bitmap->pixel_format();
    if (pixel_format == PixelFormat::y)
        pixel_format = PixelFormat::r;
    ComponentType component_type = bitmap->component_type();
    FormatFlags format_flags = FormatFlags::none;
    if (options.load_as_normalized && Struct::is_integer(component_type))
        format_flags = FormatFlags::normalized;

    // Check if bitmap is RGB and we can convert to RGBA.
    bool convert_to_rgba = false;
    if (options.extend_alpha && pixel_format == PixelFormat::rgb) {
        bool rgb_format_supported
            = FORMAT_TABLE.find(make_key(PixelFormat::rgb, component_type, format_flags)) != FORMAT_TABLE.end();
        bool rgba_format_supported
            = FORMAT_TABLE.find(make_key(PixelFormat::rgba, component_type, format_flags)) != FORMAT_TABLE.end();
        if (!rgb_format_supported && rgba_format_supported) {
            convert_to_rgba = true;
            pixel_format = PixelFormat::rgba;
        }
    }

    // Use sRGB format if requested and supported.
    if (options.load_as_srgb && pixel_format == PixelFormat::rgba && component_type == ComponentType::uint8
        && bitmap->srgb_gamma())
        format_flags = FormatFlags::srgb;

    // Find texture format.
    auto it = FORMAT_TABLE.find(make_key(pixel_format, component_type, format_flags));
    if (it == FORMAT_TABLE.end())
        SGL_THROW("Unsupported bitmap format: {} {}", pixel_format, component_type);

    return {it->second, convert_to_rgba};
}

inline ref<Bitmap> load_bitmap(const std::filesystem::path& path)
{
    return ref(new Bitmap(path));
}

inline std::pair<ref<Bitmap>, Format> convert_bitmap(ref<Bitmap> bitmap, const TextureLoader::Options& options)
{
    auto [format, convert_to_rgba] = determine_texture_format(bitmap, options);
    if (convert_to_rgba)
        bitmap = bitmap->convert(Bitmap::PixelFormat::rgba, bitmap->component_type(), bitmap->srgb_gamma());
    return {bitmap, format};
}

inline std::pair<ref<Bitmap>, Format>
load_and_convert_bitmap(const std::filesystem::path& path, const TextureLoader::Options& options)
{
    ref<Bitmap> bitmap = load_bitmap(path);
    auto [converted_bitmap, format] = convert_bitmap(bitmap, options);
    bitmap = converted_bitmap;
    return {bitmap, format};
}

inline ref<Texture> create_texture(
    Device* device,
    Blitter* blitter,
    CommandBuffer* command_buffer,
    const Bitmap* bitmap,
    Format format,
    const TextureLoader::Options& options
)
{
    bool allocate_mips = options.allocate_mips || options.generate_mips;

    ResourceUsage usage = options.usage;
    if (options.generate_mips)
        usage |= ResourceUsage::render_target;

    ref<Texture> texture = device->create_texture({
        .format = format,
        .width = bitmap->width(),
        .height = bitmap->height(),
        .mip_count = allocate_mips ? 0u : 1u,
        .usage = usage,
    });

    SubresourceData subresource_data{
        .data = bitmap->data(),
        .row_pitch = bitmap->width() * bitmap->bytes_per_pixel(),
    };

    command_buffer->upload_texture_data(texture, 0, subresource_data);
    if (options.generate_mips) {
        blitter->generate_mips(command_buffer, texture);
        texture->invalidate_views();
    }

    return texture;
}

inline std::vector<ref<Texture>> create_textures(
    Device* device,
    Blitter* blitter,
    std::span<std::future<std::pair<ref<Bitmap>, Format>>> bitmap_and_formats,
    const TextureLoader::Options& options
)
{
    std::vector<ref<Texture>> textures(bitmap_and_formats.size());
    ref<CommandBuffer> command_buffer = device->create_command_buffer();
    for (size_t i = 0; i < bitmap_and_formats.size(); ++i) {
        auto [bitmap, format] = bitmap_and_formats[i].get();
        textures[i] = create_texture(device, blitter, command_buffer, bitmap, format, options);
        if (i && (i % BATCH_SIZE == 0)) {
            command_buffer->submit();
            device->run_garbage_collection();
            command_buffer->open();
        }
    }
    command_buffer->submit();

    return textures;
}

inline ref<Texture> create_texture_array(
    Device* device,
    Blitter* blitter,
    std::span<std::future<std::pair<ref<Bitmap>, Format>>> bitmap_and_formats,
    const TextureLoader::Options& options
)
{
    SGL_ASSERT(bitmap_and_formats.size() > 0);

    bool allocate_mips = options.allocate_mips || options.generate_mips;

    ResourceUsage usage = options.usage;
    if (options.generate_mips)
        usage |= ResourceUsage::render_target;

    ref<Texture> texture;
    uint32_t first_width = 0;
    uint32_t first_height = 0;
    Format first_format = Format::unknown;

    ref<CommandBuffer> command_buffer = device->create_command_buffer();

    for (size_t i = 0; i < bitmap_and_formats.size(); ++i) {
        auto [bitmap, format] = bitmap_and_formats[i].get();

        if (i == 0) {
            texture = device->create_texture({
                .format = format,
                .width = bitmap->width(),
                .height = bitmap->height(),
                .array_size = narrow_cast<uint32_t>(bitmap_and_formats.size()),
                .mip_count = allocate_mips ? 0u : 1u,
                .usage = usage,
            });
            first_width = bitmap->width();
            first_height = bitmap->height();
            first_format = format;
        } else {
            if (bitmap->width() != first_width || bitmap->height() != first_height || format != first_format)
                SGL_THROW("Texture array requires all bitmaps to have the same dimensions and format");
        }

        if (i && (i % BATCH_SIZE == 0)) {
            command_buffer->submit();
            device->run_garbage_collection();
            command_buffer->open();
        }

        uint32_t subresource = texture->get_subresource_index(0, narrow_cast<uint32_t>(i));
        SubresourceData subresource_data{
            .data = bitmap->data(),
            .size = bitmap->buffer_size(),
            .row_pitch = bitmap->width() * bitmap->bytes_per_pixel(),
        };
        command_buffer->upload_texture_data(texture, subresource, subresource_data);

        if (options.generate_mips)
            blitter->generate_mips(command_buffer, texture, narrow_cast<uint32_t>(i));
    }
    command_buffer->submit();

    if (options.generate_mips)
        texture->invalidate_views();

    return texture;
}

TextureLoader::Options::Options() { }

TextureLoader::TextureLoader(ref<Device> device)
    : m_device(std::move(device))
{
    m_blitter = ref(new Blitter(m_device));
}

TextureLoader::~TextureLoader() = default;

ref<Texture> TextureLoader::load_texture(const Bitmap* bitmap, std::optional<Options> options_)
{
    Options options = options_.value_or(Options{});
    auto [converted_bitmap, format] = convert_bitmap(ref(const_cast<Bitmap*>(bitmap)), options);
    ref<CommandBuffer> command_buffer = m_device->create_command_buffer();
    ref<Texture> texture = create_texture(m_device, m_blitter, command_buffer, converted_bitmap, format, options);
    command_buffer->submit();
    return texture;
}

ref<Texture> TextureLoader::load_texture(const std::filesystem::path& path, std::optional<Options> options)
{
    return load_texture(ref(new Bitmap(path)), options);
}

std::vector<ref<Texture>>
TextureLoader::load_textures(std::span<const Bitmap*> bitmaps, std::optional<Options> options_)
{
    Options options = options_.value_or(Options{});

    // Convert bitmaps in parallel.
    std::vector<std::future<std::pair<ref<Bitmap>, Format>>> bitmap_and_formats;
    bitmap_and_formats.reserve(bitmaps.size());
    for (const auto& bitmap : bitmaps)
        bitmap_and_formats.push_back(thread::do_async(convert_bitmap, ref(const_cast<Bitmap*>(bitmap)), options));

    return create_textures(m_device, m_blitter, bitmap_and_formats, options);
}

std::vector<ref<Texture>>
TextureLoader::load_textures(std::span<std::filesystem::path> paths, std::optional<Options> options_)
{
    Options options = options_.value_or(Options{});

    // Load & convert bitmaps in parallel.
    std::vector<std::future<std::pair<ref<Bitmap>, Format>>> bitmap_and_formats;
    bitmap_and_formats.reserve(paths.size());
    for (const auto& path : paths)
        bitmap_and_formats.push_back(thread::do_async(load_and_convert_bitmap, path, options));

    return create_textures(m_device, m_blitter, bitmap_and_formats, options);
}

ref<Texture> TextureLoader::load_texture_array(std::span<const Bitmap*> bitmaps, std::optional<Options> options_)
{
    if (bitmaps.empty())
        return nullptr;

    Options options = options_.value_or(Options{});

    // Convert bitmaps in parallel.
    std::vector<std::future<std::pair<ref<Bitmap>, Format>>> bitmap_and_formats;
    bitmap_and_formats.reserve(bitmaps.size());
    for (const auto& bitmap : bitmaps)
        bitmap_and_formats.push_back(thread::do_async(convert_bitmap, ref(const_cast<Bitmap*>(bitmap)), options));

    return create_texture_array(m_device, m_blitter, bitmap_and_formats, options);
}

ref<Texture> TextureLoader::load_texture_array(std::span<std::filesystem::path> paths, std::optional<Options> options_)
{
    if (paths.empty())
        return nullptr;

    Options options = options_.value_or(Options{});

    // Load & convert bitmaps in parallel.
    std::vector<std::future<std::pair<ref<Bitmap>, Format>>> bitmap_and_formats;
    bitmap_and_formats.reserve(paths.size());
    for (const auto& path : paths)
        bitmap_and_formats.push_back(thread::do_async(load_and_convert_bitmap, path, options));

    return create_texture_array(m_device, m_blitter, bitmap_and_formats, options);
}

} // namespace sgl

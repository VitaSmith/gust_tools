/*
  gust_g1t - DDS texture unpacker for Gust (Koei/Tecmo) .g1t files
  Copyright © 2019-2023 VitaSmith

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include "utf8.h"
#include "util.h"
#include "parson.h"
#include "dds.h"

#define JSON_VERSION            2
#define G1TG_MAGIC              0x47315447        // 'G1TG'
#define REPORT_URL              "https://github.com/VitaSmith/gust_tools/issues"

// Known flags
#define G1T_FLAG_STANDARD_FLAGS 0x000000011200ULL // Flags that are commonly set
#define G1T_FLAG_EXTENDED_DATA  0x000000000001ULL // Set if the texture has local data in the texture entry.
#define G1T_FLAG_SRGB           0x000000002000ULL // Set if the texture uses sRGB
#define G1T_FLAG_DOUBLE_HEIGHT  0x001000000000ULL // Some textures need to be doubled in height
#define G1T_FLAG_NORMAL_MAP     0x030000000000ULL // Usually set for normal maps (but not always)
#define G1T_FLAG_SURFACE_TEX    0x000000000001ULL // Set for textures that appear on a model's surface
#define G1T_FLAG_TEXTURE_ARRAY  0x0000F00F0000ULL
// This one is not used in G1Ts, but only in this application
#define G1T_FLAG_CUBE_MAP       0x000100000000ULL

// Known platforms
#define SONY_PS2                0x00
#define SONY_PS3                0x01
#define MICROSOFT_X360          0x02
#define NINTENDO_WII            0x03
#define NINTENDO_DS             0x04
#define NINTENDO_3DS            0x05
#define SONY_PSV                0x06
#define GOOGLE_ANDROID          0x07
#define APPLE_IOS               0x08
#define NINTENDO_WIIU           0x09
#define MICROSOFT_WINDOWS       0x0A
#define SONY_PS4                0x0B
#define MICROSOFT_XONE          0x0C
#define NINTENDO_SWITCH         0x10

#pragma pack(push, 1)
typedef struct {
    uint32_t    magic;
    uint32_t    version;
    uint32_t    total_size;
    uint32_t    header_size;
    uint32_t    nb_textures;
    uint32_t    platform;       // See the platforms list above
    uint32_t    extra_size;
} g1t_header;

// This is followed by uint32_t global_flags[nb_textures]
// This is followed by uint32_t offset[nb_textures]
// This is (optionally) followed by an array of global extra data (for textures with G1T_FLAG_GLOBAL_XDATA)

typedef struct {
    uint8_t     z_mipmaps : 4;
    uint8_t     mipmaps : 4;
    uint8_t     type;
    uint8_t     dx : 4;
    uint8_t     dy : 4;
    uint8_t     flags[5];
} g1t_tex_header;

// May be followed by a data[] section of 12, 16 or 20 bytes where the 32-bit
// words at positions 12 and 16 are the actual texture width and height.
// The 32-bit float at pos 4 is a depth, and the other words contain the
// number of frames in a texture array as well as flags.

#pragma pack(pop)

// Same order as enum DDS_FORMAT
const char* argb_name[] = { NULL, "ABGR", "ARGB", "GRAB", "RGBA",
                                  "ABGR", "ARGB", "GRAB", "RGBA" };

static inline const char* platform_to_name(uint32_t platform)
{
    switch (platform) {
    case SONY_PS2:
        return "PS2";
    case SONY_PS3:
        return "PS3";
    case MICROSOFT_X360:
        return "Xbox 360";
    case NINTENDO_WII:
        return "Wii";
    case NINTENDO_DS:
        return "DS";
    case NINTENDO_3DS:
        return "3DS";
    case SONY_PSV:
        return "Vita";
    case GOOGLE_ANDROID:
        return "Android";
    case APPLE_IOS:
        return "iOS";
    case NINTENDO_WIIU:
        return "WiiU";
    case MICROSOFT_WINDOWS:
        return "Windows";
    case SONY_PS4:
        return "PS4";
    case MICROSOFT_XONE:
        return "Xbox One";
    case NINTENDO_SWITCH:
        return "Switch";
    default:
        return NULL;
    }
}

static inline uint32_t name_to_platform(const char* name)
{
    if (name == NULL)
        return UINT32_MAX;
    if (stricmp(name, "PS2") == 0)
        return SONY_PS2;
    if (stricmp(name, "PS3") == 0)
        return SONY_PS3;
    if (stricmp(name, "Xbox 360") == 0)
        return MICROSOFT_X360;
    if (stricmp(name, "Wii") == 0)
        return NINTENDO_WII;
    if (stricmp(name, "DS") == 0)
        return NINTENDO_DS;
    if (stricmp(name, "3DS") == 0)
        return NINTENDO_3DS;
    if (stricmp(name, "Vita") == 0)
        return SONY_PSV;
    if (stricmp(name, "Android") == 0)
        return GOOGLE_ANDROID;
    if (stricmp(name, "iOS") == 0)
        return APPLE_IOS;
    if (stricmp(name, "WiiU") == 0)
        return NINTENDO_WIIU;
    if (stricmp(name, "Windows") == 0)
        return MICROSOFT_WINDOWS;
    if (stricmp(name, "PS4") == 0)
        return SONY_PS4;
    if (stricmp(name, "Xbox One") == 0)
        return MICROSOFT_XONE;
    if (stricmp(name, "Switch") == 0)
        return NINTENDO_SWITCH;
    return UINT32_MAX;
}

static void json_to_flags(uint64_t* flags, JSON_Array* json_flags_array)
{
    memset(flags, 0, 2 * sizeof(uint64_t));
    for (size_t i = 0; i < json_array_get_count(json_flags_array); i++) {
        const char* flag_str = json_array_get_string(json_flags_array, i);
        // Named flags
        if (strcmp(flag_str, "STANDARD_FLAGS") == 0)
            flags[0] |= G1T_FLAG_STANDARD_FLAGS;
        else if (strcmp(flag_str, "NORMAL_MAP") == 0)
            flags[0] |= G1T_FLAG_NORMAL_MAP;
        else if (strcmp(flag_str, "SRGB_COLORSPACE") == 0)
            flags[0] |= G1T_FLAG_SRGB;
        else if (strcmp(flag_str, "EXTENDED_DATA") == 0)
            flags[0] |= G1T_FLAG_EXTENDED_DATA;
        else if (strcmp(flag_str, "DOUBLE_HEIGHT") == 0)
            flags[0] |= G1T_FLAG_DOUBLE_HEIGHT;
        else if (strcmp(flag_str, "SURFACE_TEXTURE") == 0)
            flags[1] |= G1T_FLAG_SURFACE_TEX;
        // Unnamed flags
        else if (strncmp(flag_str, "FLAG_", 5) == 0) {
            int val = atoi(&flag_str[5]);
            flags[0 + val / 64] |= 1ULL << (val % 64);
        } else if (strcmp(flag_str, "TEXTURE_ARRAY") != 0 &&
            strcmp(flag_str, "CUBE_MAP") != 0) {
            fprintf(stderr, "ERROR: Unsupported JSON flag '%s'\n", flag_str);
        }
    }
}

#define CHECK_MASK(flags, mask, array, string) \
  if ((flags & (mask)) == (mask)) { flags &= ~(mask); json_array_append_string(json_array(array), string); }

#define GET_NB_FRAMES(val) ((((val) >> 28) & 0x0f) + (((val) >> 12) & 0xf0))

static JSON_Value* flags_to_json(uint64_t* flags)
{
    JSON_Value* json_flags_array = json_value_init_array();
    static char str[64] = { 0 };
    uint64_t flags_copy[2] = { flags[0], flags[1] };

    // Named flags
    CHECK_MASK(flags_copy[0], G1T_FLAG_STANDARD_FLAGS, json_flags_array, "STANDARD_FLAGS");
    // A value of 3 in the extra flags seems to be associated
    // with a normal map... but not always (e.g. BR2's pc000_scl)
    CHECK_MASK(flags_copy[0], G1T_FLAG_NORMAL_MAP, json_flags_array, "NORMAL_MAP");
    CHECK_MASK(flags_copy[0], G1T_FLAG_SRGB, json_flags_array, "SRGB_COLORSPACE");
    CHECK_MASK(flags_copy[0], G1T_FLAG_EXTENDED_DATA, json_flags_array, "EXTENDED_DATA");
    CHECK_MASK(flags_copy[0], G1T_FLAG_DOUBLE_HEIGHT, json_flags_array, "DOUBLE_HEIGHT");
    CHECK_MASK(flags_copy[1], G1T_FLAG_SURFACE_TEX, json_flags_array, "SURFACE_TEXTURE");
    if (flags_copy[1] & G1T_FLAG_TEXTURE_ARRAY) {
        json_array_append_string(json_array(json_flags_array), "TEXTURE_ARRAY");
        flags_copy[1] &= ~G1T_FLAG_TEXTURE_ARRAY;
    }
    CHECK_MASK(flags_copy[1], G1T_FLAG_CUBE_MAP, json_flags_array, "CUBE_MAP");

    // Unnamed flags
    for (int i = 0; i < 2; i++) {
        uint64_t mask = 1ULL;
        for (int j = 0; j < 64; j++) {
            if (flags_copy[i] & mask) {
                snprintf(str, sizeof(str), "FLAG_%03d", 64 * i + j);
                json_array_append_string(json_array(json_flags_array), str);
            }
            mask <<= 1;
        }
    }
    return json_flags_array;
}

static size_t write_dds_header(FILE* fd, enum DDS_FORMAT format, uint32_t width,
                               uint32_t height, uint32_t mipmaps, uint64_t* flags)
{
    if ((fd == NULL) || (width == 0) || (height == 0))
        return 0;

    DDS_HEADER header = { 0 };
    uint32_t bpp = dds_bpp(format);
    bool use_dx10 = (format == DDS_FORMAT_BC7) || (format == DDS_FORMAT_DX10) ||
        (flags[0] & G1T_FLAG_SRGB) || (flags[1] & G1T_FLAG_TEXTURE_ARRAY);
    header.size = 124;
    header.flags = DDS_HEADER_FLAGS_TEXTURE | DDS_HEADER_FLAGS_LINEARSIZE;
    header.height = height;
    header.width = width;
    // Gimp complains when this is not set
    if (dds_bpb(format) >= 8)
        header.pitchOrLinearSize = ((width + 3) / 4) * ((height + 3) / 4) * dds_bpb(format);
    else
        header.pitchOrLinearSize = width * height * dds_bpb(format);
    header.ddspf.size = 32;
    if (format == DDS_FORMAT_BGR8) {
        header.ddspf.flags = DDS_RGB;
        header.ddspf.RGBBitCount = bpp;
        switch (bpp) {
        case 24:
            header.ddspf.RBitMask = 0x00ff0000;
            header.ddspf.GBitMask = 0x0000ff00;
            header.ddspf.BBitMask = 0x000000ff;
            break;
        default:
            fprintf(stderr, "ERROR: Unsupported bits-per-pixel value %d\n", bpp);
            return 0;
        }
    } else if (format >= DDS_FORMAT_ABGR4 && format <= DDS_FORMAT_RGBA8) {
        if (use_dx10) {
            header.ddspf.flags = DDS_FOURCC;
            header.ddspf.fourCC = get_fourCC(DDS_FORMAT_DX10);
        } else {
            header.ddspf.flags = DDS_RGBA;
        }
        header.ddspf.RGBBitCount = bpp;
        // Always save as ARGB, to keep VS, Gimp and Photoshop happy
        switch (bpp) {
        case 16:
            header.ddspf.RBitMask = 0x00000f00;
            header.ddspf.GBitMask = 0x000000f0;
            header.ddspf.BBitMask = 0x0000000f;
            header.ddspf.ABitMask = 0x0000f000;
            break;
        case 32:
            header.ddspf.RBitMask = 0x00ff0000;
            header.ddspf.GBitMask = 0x0000ff00;
            header.ddspf.BBitMask = 0x000000ff;
            header.ddspf.ABitMask = 0xff000000;
            break;
        // I have absolutely no idea if the following will work...
        case 64:
            header.ddspf.RBitMask = 0x0000ffff;
            header.ddspf.GBitMask = 0xffff0000;
            header.ddspf.BBitMask = 0x0000ffff;
            header.ddspf.ABitMask = 0xffff0000;
            break;
        case 128:
            header.ddspf.RBitMask = 0xffffffff;
            header.ddspf.GBitMask = 0xffffffff;
            header.ddspf.BBitMask = 0xffffffff;
            header.ddspf.ABitMask = 0xffffffff;
            break;
        default:
            fprintf(stderr, "ERROR: Unsupported bits-per-pixel value %d\n", bpp);
            return 0;
        }
    } else if (format == DDS_FORMAT_R8) {
        header.ddspf.flags = DDS_RGBA;
        header.ddspf.RGBBitCount = bpp;
        header.ddspf.RBitMask = (uint32_t)((1ULL << bpp) - 1);
    } else if (format == DDS_FORMAT_ARGB32) {
        header.ddspf.flags = DDS_FOURCC;
        // 128bpp RGBA float
        header.ddspf.fourCC = 0x74;
        use_dx10 = false;
    } else if (format == DDS_FORMAT_ARGB16) {
        header.ddspf.flags = DDS_FOURCC;
        // 64bpp RGBA half-float
        header.ddspf.fourCC = 0x71; // Or it may be 0x24 if using R16G16B16A16
        use_dx10 = false;
    } else {
        header.ddspf.flags = DDS_FOURCC;
        header.ddspf.fourCC = get_fourCC(use_dx10 ? DDS_FORMAT_DX10 : format);
    }
    header.caps = DDS_SURFACE_FLAGS_TEXTURE;
    if (mipmaps != 0) {
        header.mipMapCount = mipmaps;
        header.flags |= DDS_HEADER_FLAGS_MIPMAP;
        header.caps |= DDS_SURFACE_FLAGS_MIPMAP;
    }
    if (flags[1] & G1T_FLAG_CUBE_MAP) {
        header.caps |= DDS_SURFACE_FLAGS_CUBEMAP;
        header.caps2 |= DDS_CUBEMAP_ALLFACES;
    }
    if (flags[0] & G1T_FLAG_NORMAL_MAP)
        header.ddspf.flags |= DDS_NORMAL;
    size_t r = fwrite(&header, sizeof(DDS_HEADER), 1, fd);
    if (r != 1)
        return r;
    if (use_dx10) {
        DDS_HEADER_DXT10 dxt10_hdr = { 0 };
        dxt10_hdr.resourceDimension = D3D10_RESOURCE_DIMENSION_TEXTURE2D;
        dxt10_hdr.arraySize = GET_NB_FRAMES(flags[1]);
        if (dxt10_hdr.arraySize == 0)
            dxt10_hdr.arraySize = 1;
        dxt10_hdr.miscFlag = (flags[1] & G1T_FLAG_CUBE_MAP) ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
        switch (format) {
        case DDS_FORMAT_DXT1:
            dxt10_hdr.dxgiFormat = (flags[0] & G1T_FLAG_SRGB) ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
            break;
        case DDS_FORMAT_DXT3:
            dxt10_hdr.dxgiFormat = (flags[0] & G1T_FLAG_SRGB) ? DXGI_FORMAT_BC2_UNORM_SRGB : DXGI_FORMAT_BC2_UNORM;
            break;
        case DDS_FORMAT_DXT5:
            dxt10_hdr.dxgiFormat = (flags[0] & G1T_FLAG_SRGB) ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
            break;
        case DDS_FORMAT_BC4:
            dxt10_hdr.dxgiFormat = (flags[0] & G1T_FLAG_SRGB) ? DXGI_FORMAT_BC4_SNORM : DXGI_FORMAT_BC4_UNORM;
            break;
        case DDS_FORMAT_BC7:
        case DDS_FORMAT_DX10:
            dxt10_hdr.dxgiFormat = (flags[0] & G1T_FLAG_SRGB) ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
            break;
        case DDS_FORMAT_BC6H:
            dxt10_hdr.dxgiFormat = (flags[0] & G1T_FLAG_SRGB) ? DXGI_FORMAT_BC6H_SF16 : DXGI_FORMAT_BC6H_UF16;
            break;
        case DDS_FORMAT_RGBA8:
        case DDS_FORMAT_ARGB8:
            dxt10_hdr.dxgiFormat = (flags[0] & G1T_FLAG_SRGB) ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
        default:
            assert(false);
            break;
        }
        r = fwrite(&dxt10_hdr, sizeof(DDS_HEADER_DXT10), 1, fd);
    }
    return r;
}

static void rgba_convert(const enum DDS_FORMAT format, const char* in,
                         const char* out, uint8_t* buf, const uint32_t size)
{
    uint32_t bits_per_pixel = dds_bpp(format);
    assert(bits_per_pixel % 8 == 0);
    assert(format >= DDS_FORMAT_ABGR4 && format <= DDS_FORMAT_RGBA8);

    const int rgba[4] = { 'R', 'G', 'B', 'A' };
    if (strcmp(in, out) == 0)
        return;

    uint32_t mask[4];
    int rot[4];
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t pos_in = 3 - (uint32_t)((uintptr_t)strchr(in, rgba[i]) - (uintptr_t)in);
        uint32_t pos_out = 3 - (uint32_t)((uintptr_t)strchr(out, rgba[i]) - (uintptr_t)out);
        mask[i] = ((1 << (bits_per_pixel / 4)) - 1) << (pos_in * 8);
        rot[i] = (pos_out - pos_in) * 8;
    }

    for (uint32_t j = 0; j < size; j += 4) {
        uint32_t s;
        switch (bits_per_pixel) {
        case 16: s = getbe16(&buf[j]); break;
        case 24: s = getbe24(&buf[j]); break;
        default: s = getbe32(&buf[j]); break;
        }
        uint32_t d = 0;
        for (uint32_t i = 0; i < 4; i++)
            d |= (rot[i] > 0) ? ((s & mask[i]) << rot[i]) : ((s & mask[i]) >> -rot[i]);
        switch (bits_per_pixel) {
        case 16: setbe16(&buf[j], (uint16_t)d); break;
        case 24: setbe24(&buf[j], d); break;
        default: setbe32(&buf[j], d); break;
        }
    }
}

// "Inflate" a 32 bit value by interleaving 0 bits at odd positions.
static __inline uint32_t inflate_bits(uint32_t x)
{
    x &= 0x0000FFFF;
    x = (x | (x << 8))  & 0x00FF00FF;
    x = (x | (x << 4))  & 0x0F0F0F0F;
    x = (x | (x << 2))  & 0x33333333;
    x = (x | (x << 1))  & 0x55555555;
    return x;
}

// "Deflate" a 32-bit value by deinterleaving all odd bits.
static __inline uint32_t deflate_bits(uint32_t x)
{
    x &= 0x55555555;
    x = (x | (x >> 1))  & 0x33333333;
    x = (x | (x >> 2))  & 0x0F0F0F0F;
    x = (x | (x >> 4))  & 0x00FF00FF;
    x = (x | (x >> 8))  & 0x0000FFFF;
    return x;
}

// From two 32-bit (x,y) coordinates, compute the 64-bit Morton (or Z-order) value.
static __inline uint32_t xy_to_morton(uint32_t x, uint32_t y)
{
    return (inflate_bits(x) << 1) | (inflate_bits(y) << 0);
}

// Fom a 32-bit Morton (Z-order) value, recover two 32-bit (x,y) coordinates.
static __inline void morton_to_xy(uint32_t z, uint32_t* x, uint32_t* y)
{
    *x = deflate_bits(z >> 1);
    *y = deflate_bits(z >> 0);
}

// Apply or reverse a Morton transformation, a.k.a. a Z-order curve, to a texture.
// If morton_order is negative, a reverse Morton transformation is applied.
static void mortonize(const enum DDS_FORMAT format, const int16_t morton_order,
                      uint32_t width, uint32_t height, uint8_t* buf, const uint32_t size, uint32_t wf)
{
    const uint32_t bits_per_element = dds_bpp(format) * dds_bwh(format) * dds_bwh(format) * wf;
    const uint32_t bytes_per_element = bits_per_element / 8;
    width /= dds_bwh(format) * wf;
    height /= dds_bwh(format);
    uint32_t num_elements = size / bytes_per_element;
    uint16_t k = (uint16_t)abs(morton_order);
    bool reverse = (morton_order != (int16_t)k);

    // Only deal with elements that are multiple of one byte in size
    assert(bits_per_element % 8 == 0);
    // Validate that the size of the buffer matches the dimensions provided
    assert(bytes_per_element * width * height == size);
    // Only deal with texture that are smaller than 64k*64k
    assert((width < 0x10000) && (height < 0x10000));
    // Ensure that width and height are an exact multiple of 2^k
    assert(width % (1 << k) == 0);
    assert(height % (1 << k) == 0);
    // Ensure that we won't produce x or y that are larger than the maximum dimension
    assert(k <= log2(max(width, height)));
    uint32_t tile_width = 1 << k;
    uint32_t tile_size = tile_width * tile_width;
    uint32_t mask = tile_size - 1;
    uint8_t* tmp_buf = (uint8_t*)malloc(size);
    for (uint32_t i = 0; i < num_elements; i++) {
        uint32_t j, x, y;
        if (reverse) {  // Morton value to an (x,y) pair
            // Recover (x,y) for the Morton tile
            morton_to_xy(i & mask, &x, &y);
            // Now apply untiling by offsetting (x,y) with the tile positiom
            x += ((i / tile_size) % (width / tile_width)) * tile_width;
            y += ((i / tile_size) / (width / tile_width)) * tile_width;
            j = y * width + x;
        } else {        // Morton value from an (x,y) pair
            x = i % width; y = i / width;
            j = xy_to_morton(x, y) & mask;
            // Now, apply tiling. This is accomplished by offseting our value
            // with the current tile position multiplied by the tile size.
            j += ((y / tile_width) * (width / tile_width) + (x / tile_width)) * tile_size;
        }
        assert(j < num_elements);
        memcpy(&tmp_buf[j * bytes_per_element], &buf[i * bytes_per_element], bytes_per_element);
    }
    memcpy(buf, tmp_buf, size);
    free(tmp_buf);
}

static void tile(const enum DDS_FORMAT format, uint32_t tile_size, uint32_t width,
                 uint8_t* buf, const uint32_t size)
{
    const uint32_t bytes_per_element = dds_bpb(format);
    assert(tile_size % dds_bwh(format) == 0);
    assert(width % dds_bwh(format) == 0);
    tile_size /= dds_bwh(format);
    width /= dds_bwh(format);
    assert(bytes_per_element != 0);
    assert(size % bytes_per_element == 0);
    assert(size % (tile_size * tile_size) == 0);
    assert(width % tile_size == 0);

    uint8_t* tmp_buf = (uint8_t*)malloc(size);

    for (uint32_t i = 0; i < size / bytes_per_element / tile_size / tile_size; i++) {
        uint32_t tile_row = i / (width / tile_size);
        uint32_t tile_column = i % (width / tile_size);
        uint32_t tile_start = tile_row * width * tile_size + tile_column * tile_size;
        for (uint32_t j = 0; j < tile_size; j++) {
            memcpy(&tmp_buf[bytes_per_element * (i * tile_size * tile_size + j * tile_size)],
                &buf[bytes_per_element * (tile_start + j * width)],
                (size_t)tile_size * bytes_per_element);
        }
    }

    memcpy(buf, tmp_buf, size);
    free(tmp_buf);
}

static void untile(const enum DDS_FORMAT format, uint32_t tile_size, uint32_t width,
                   uint8_t* buf, const uint32_t size)
{
    const uint32_t bytes_per_element = dds_bpb(format);
    assert(tile_size % dds_bwh(format) == 0);
    assert(width % dds_bwh(format) == 0);
    tile_size /= dds_bwh(format);
    width /= dds_bwh(format);
    assert(bytes_per_element != 0);
    assert(size % bytes_per_element == 0);
    assert(size % (tile_size * tile_size) == 0);
    assert(width % tile_size == 0);

    uint8_t* tmp_buf = (uint8_t*)malloc(size);

    for (uint32_t i = 0; i < size / bytes_per_element / tile_size / tile_size; i++) {
        uint32_t tile_row = i / (width / tile_size);
        uint32_t tile_column = i % (width / tile_size);
        uint32_t tile_start = tile_row * width * tile_size + tile_column * tile_size;
        for (uint32_t j = 0; j < tile_size; j++) {
            memcpy(&tmp_buf[bytes_per_element * (tile_start + j * width)],
                &buf[bytes_per_element * (i * tile_size * tile_size + j * tile_size)],
                (size_t)tile_size * bytes_per_element);
        }
    }

    memcpy(buf, tmp_buf, size);
    free(tmp_buf);
}

static void flip(uint32_t bits_per_pixel, uint8_t* buf, const uint32_t size, uint32_t width)
{
    assert(bits_per_pixel % 8 == 0);
    const uint32_t line_size = width * (bits_per_pixel / 8);
    assert(size % line_size == 0);
    const uint32_t max_line = (size / line_size) - 1;

    uint8_t* tmp_buf = (uint8_t*)malloc(size);

    for (uint32_t i = 0; i <= max_line; i++)
        memcpy(&tmp_buf[i * line_size], &buf[(max_line - i) * line_size], line_size);

    memcpy(buf, tmp_buf, size);
    free(tmp_buf);
}

int main_utf8(int argc, char** argv)
{
    int r = -1;
    FILE *file = NULL;
    uint8_t* buf = NULL;
    uint32_t *offset_table = NULL, *flag_table = NULL;
    uint32_t magic;
    char path[256], *dir = NULL;
    JSON_Value* json = NULL;
    bool list_only = (argc == 3) && (argv[1][0] == '-') && (argv[1][1] == 'l');
    bool flip_image = (argc == 3) && (argv[1][0] == '-') && (argv[1][1] == 'f');
    bool no_prompt = (argc == 3) && (argv[1][0] == '-') && (argv[1][1] == 'y');

    if ((argc != 2) && !list_only && !flip_image && !no_prompt) {
        printf("%s %s (c) 2019-2023 VitaSmith\n\n"
            "Usage: %s [-l] [-f] [-y] <file or directory>\n\n"
            "Extracts (file) or recreates (directory) a Gust .g1t texture archive.\n\n"
            "Note: A backup (.bak) of the original is automatically created, when the target\n"
            "is being overwritten for the first time.\n",
            _appname(argv[0]), GUST_TOOLS_VERSION_STR, _appname(argv[0]));
        return 0;
    }

    if (is_directory(argv[argc - 1])) {
        if (list_only) {
            fprintf(stderr, "ERROR: Option -l is not supported when creating an archive\n");
            goto out;
        }
        snprintf(path, sizeof(path), "%s%cg1t.json", argv[argc - 1], PATH_SEP);
        if (!is_file(path)) {
            fprintf(stderr, "ERROR: '%s' does not exist\n", path);
            goto out;
        }
        json = json_parse_file_with_comments(path);
        if (json == NULL) {
            fprintf(stderr, "ERROR: Can't parse JSON data from '%s'\n", path);
            goto out;
        }
        const uint32_t json_version = json_object_get_uint32(json_object(json), "json_version");
        if (json_version != JSON_VERSION) {
            fprintf(stderr, "ERROR: This utility is not compatible with the JSON file provided.\n"
                "You need to (re)extract the '.g1t' using this application.\n");
            goto out;
        }
        const char* filename = json_object_get_string(json_object(json), "name");
        uint32_t version = json_object_get_uint32(json_object(json), "version");
        if ((filename == NULL) || (version == 0) || (version > 10000))
            goto out;
        JSON_Array* json_textures_array = json_object_get_array(json_object(json), "textures");
        if (json_textures_array == NULL) {
            fprintf(stderr, "ERROR: Invalid or missing JSON texture array\n");
            goto out;
        }
        JSON_Array* json_extra_data_array = json_object_get_array(json_object(json), "extra_data");

        strcpy(path, argv[argc - 1]);
        if (get_trailing_slash(path) != 0)
            path[get_trailing_slash(path)] = 0;
        else
            path[0] = 0;
        strcat(path, filename);
        path[sizeof(path) - 1] = 0;
        printf("Creating '%s'...\n", path);
        create_backup(path);
        file = fopen_utf8(path, "wb+");
        if (file == NULL) {
            fprintf(stderr, "ERROR: Can't create file '%s'\n", path);
            goto out;
        }
        g1t_header hdr = { 0 };
        if (name_to_platform(json_object_get_string(json_object(json), "platform")) == UINT32_MAX)
            hdr.platform = json_object_get_uint32(json_object(json), "platform");
        else
            hdr.platform = name_to_platform(json_object_get_string(json_object(json), "platform"));
        if (hdr.platform == SONY_PS3 || hdr.platform == NINTENDO_WII || hdr.platform == NINTENDO_WIIU)
            data_endianness = big_endian;
        hdr.magic = G1TG_MAGIC;
        char version_string[6] = { 0 };
        snprintf(version_string, sizeof(version_string), "%04d", version);
        hdr.version = getbe32(version_string);
        hdr.total_size = 0;  // To be rewritten when we're done
        hdr.nb_textures = (uint32_t)json_array_get_count(json_textures_array);

        hdr.extra_size = (uint32_t)json_array_get_count(json_extra_data_array) * sizeof(uint16_t);
        hdr.header_size = sizeof(hdr) + hdr.nb_textures * sizeof(uint32_t);
        fix_endian32(&hdr, sizeof(hdr) / sizeof(uint32_t));
        if (fwrite(&hdr, sizeof(uint32_t), sizeof(hdr) / sizeof(uint32_t), file) != sizeof(hdr) / sizeof(uint32_t)) {
            fprintf(stderr, "ERROR: Can't write header\n");
            goto out;
        }
        fix_endian32(&hdr, sizeof(hdr) / sizeof(uint32_t));

        if (!flip_image)
            flip_image = json_object_get_boolean(json_object(json), "flip");

        // Create the placeholders for the global flags and offset table
        flag_table = calloc(hdr.nb_textures, sizeof(uint32_t));
        if (fwrite(flag_table, sizeof(uint32_t), hdr.nb_textures, file) != hdr.nb_textures) {
            fprintf(stderr, "ERROR: Can't write global flags\n");
            goto out;
        }

        offset_table = calloc(hdr.nb_textures, sizeof(uint32_t));
        offset_table[0] = hdr.nb_textures * sizeof(uint32_t);
        if (fwrite(offset_table, sizeof(uint32_t), hdr.nb_textures, file) != hdr.nb_textures) {
            fprintf(stderr, "ERROR: Can't write texture offsets\n");
            goto out;
        }

        // Deal with the global extra data array
        for (size_t i = 0; i < json_array_get_count(json_extra_data_array); i++) {
            uint16_t extra_data = getv16(json_array_get_uint16(json_extra_data_array, i));
            if (fwrite(&extra_data, 1, sizeof(uint16_t), file) != sizeof(uint16_t)) {
                fprintf(stderr, "ERROR: Can't write global extra data\n");
                goto out;
            }
        }

        printf("TYPE OFFSET     SIZE       NAME");
        dir = strdup(argv[argc - 1]);
        if (dir == NULL) {
            fprintf(stderr, "ERROR: Alloc error\n");
            goto out;
        }
        dir[get_trailing_slash(dir)] = 0;
        for (size_t i = 0; i < strlen(_basename(argv[argc - 1])); i++)
            putchar(' ');
        printf("     DIMENSIONS MIPMAPS PROPS\n");
        for (uint32_t i = 0; i < hdr.nb_textures; i++) {
            offset_table[i] = ftell(file) - hdr.header_size;
            JSON_Object* texture_entry = json_array_get_object(json_textures_array, i);
            g1t_tex_header tex = { 0 };
            tex.type = json_object_get_uint8(texture_entry, "type");
            tex.z_mipmaps = json_object_get_uint8(texture_entry, "z_mipmaps");
            const char* depth_str = json_object_get_string(texture_entry, "depth");
            float depth = (depth_str == NULL) ? 0.0f : (float)atof(depth_str);
            uint64_t flags[2];
            json_to_flags(flags, json_object_get_array(texture_entry, "flags"));
            for (size_t j = 0; j < array_size(tex.flags); j++)
                tex.flags[array_size(tex.flags) - j - 1] = (uint8_t)(flags[0] >> (8 * j));
            flag_table[i] = (uint32_t)(flags[0] >> 40);
            uint32_t nb_frames = json_object_get_uint32(texture_entry, "nb_frames");
            flags[1] |= ((uint64_t)nb_frames & 0x0f) << 28 | ((uint64_t)nb_frames & 0xf0) << 12;
            if (nb_frames == 0)
                nb_frames = 1;
            // Read the DDS file
            snprintf(path, sizeof(path), "%s%s%c%s", dir, _basename(argv[argc - 1]), PATH_SEP,
                json_object_get_string(texture_entry, "name"));
            uint32_t texture_size = read_file(path, &buf);
            if (texture_size == UINT32_MAX)
                goto out;
            if (texture_size <= sizeof(DDS_HEADER)) {
                fprintf(stderr, "ERROR: '%s' is too small\n", path);
                goto out;
            }
            if (*((uint32_t*)buf) != DDS_MAGIC) {
                fprintf(stderr, "ERROR: '%s' is not a DDS file\n", path);
                goto out;
            }
            DDS_HEADER* dds_header = (DDS_HEADER*)&buf[sizeof(uint32_t)];
            texture_size -= sizeof(uint32_t) + sizeof(DDS_HEADER);
            uint8_t* dds_payload = (uint8_t*)&buf[sizeof(uint32_t) + sizeof(DDS_HEADER)];
            // We may have a DXT10 additional header
            if (dds_header->ddspf.fourCC == get_fourCC(DDS_FORMAT_DX10)) {
                texture_size -= sizeof(DDS_HEADER_DXT10);
                dds_payload = &dds_payload[sizeof(DDS_HEADER_DXT10)];
            }
            tex.mipmaps = json_object_get_uint8(texture_entry, "mipmaps");
            if (tex.mipmaps == 0) {
                tex.mipmaps = (uint8_t)dds_header->mipMapCount;
            } else if ((uint8_t)dds_header->mipMapCount < tex.mipmaps) {
                fprintf(stderr, "WARNING: Number of mipmaps from imported texture is smaller than original\n");
                tex.mipmaps = (uint8_t)dds_header->mipMapCount;
            } else if ((uint8_t)dds_header->mipMapCount > tex.mipmaps) {
                fprintf(stderr, "NOTE: Truncating number of mipmaps from %d to %d\n", dds_header->mipMapCount, tex.mipmaps);
            }
            // Are both width and height a power of two?
            // TODO: Also check if height/width are larger than what we can represent with dx/dy
            bool po2_sizes = is_power_of_2(dds_header->width) && is_power_of_2(dds_header->height);
            if (!po2_sizes && !(flags[0] & G1T_FLAG_EXTENDED_DATA)) {
                fprintf(stderr, "ERROR: Extended data flag must be set for textures with dimensions that aren't a power of two\n");
                goto out;
            }
            if (po2_sizes) {
                tex.dx = (uint8_t)find_msb(dds_header->width);
                tex.dy = (uint8_t)find_msb(dds_header->height / ((flags[0] & G1T_FLAG_DOUBLE_HEIGHT) ? 2 : 1));
            }
            if (data_endianness == big_endian) {
                uint8_t swap_tmp = tex.dx;
                tex.dx = tex.dy;
                tex.dy = swap_tmp;
                swap_tmp = tex.z_mipmaps;
                tex.z_mipmaps = tex.mipmaps;
                tex.mipmaps = swap_tmp;
            } else {
                for (size_t j = 0; j < array_size(tex.flags); j++)
                    tex.flags[j] = tex.flags[j] >> 4 | tex.flags[j] << 4;
            }
            // Write texture header
            if (fwrite(&tex, sizeof(tex), 1, file) != 1) {
                fprintf(stderr, "ERROR: Can't write texture header\n");
                goto out;
            }
            // Swap back tex.mipmaps for the rest of our processing
            if (data_endianness != platform_endianness) {
                uint8_t swap_tmp = tex.z_mipmaps;
                tex.z_mipmaps = tex.mipmaps;
                tex.mipmaps = swap_tmp;
            }
            // Write data
            if (flags[0] & G1T_FLAG_EXTENDED_DATA) {
                uint32_t data[5], data_size;
                data[1] = getv32(*((uint32_t*)&depth));
                setbe32(&data[2], (uint32_t)flags[1]);
                data[3] = getv32(dds_header->width);
                data[4] = getv32(dds_header->height / ((flags[0] & G1T_FLAG_DOUBLE_HEIGHT) ? 2 : 1));
                if (!is_power_of_2(dds_header->width) || !is_power_of_2(dds_header->height))
                    data_size = 5;
                else
                    data_size = 3;
                data[0] = getv32(data_size * sizeof(uint32_t));
                if (fwrite(data, sizeof(uint32_t), data_size, file) != data_size) {
                    fprintf(stderr, "ERROR: Can't write extended data\n");
                    goto out;
                }
            }

            // Set the default ARGB format for the platform
            uint32_t default_texture_format;
            switch (hdr.platform) {
            case NINTENDO_DS:
            case NINTENDO_3DS:
            case SONY_PS4:
                default_texture_format = DDS_FORMAT_GRAB8;
                break;
            case SONY_PSV:
            case NINTENDO_SWITCH:
                default_texture_format = DDS_FORMAT_ARGB8;
                break;
            default:    // PC and other platforms
                default_texture_format = DDS_FORMAT_RGBA8;
                break;
            }
            uint32_t texture_format = default_texture_format;
            bool swizzled = false;
            switch (tex.type) {
            case 0x00: break;   // ???
            case 0x01: break;   // ???
            case 0x02: break;   // ???
            case 0x03: texture_format = DDS_FORMAT_ARGB16; break;
            case 0x04: texture_format = DDS_FORMAT_ARGB32; break;
            case 0x06: texture_format = DDS_FORMAT_DXT1; break; // PS2??, PS3
            case 0x07: texture_format = DDS_FORMAT_DXT3; break;
            case 0x08: texture_format = DDS_FORMAT_DXT5; break; // PS3
            case 0x09: swizzled = true; break;  // PS4
//            case 0x0A: swizzled = true; break;
            case 0x10: texture_format = DDS_FORMAT_DXT1; swizzled = true; break;    // PSV
            case 0x11: texture_format = DDS_FORMAT_DXT3; swizzled = true; break;    // PSV
            case 0x12: texture_format = DDS_FORMAT_DXT5; swizzled = true; break;    // PSV
            case 0x21: break;   // Switch
            // 0x3C and 0x3D are definitely 16bpp, but after that...
            case 0x3C: texture_format = DDS_FORMAT_ARGB4; break; // 3DS
            case 0x3D: texture_format = DDS_FORMAT_ARGB4; break; // 3DS
            case 0x45: texture_format = DDS_FORMAT_BGR8; swizzled = true; break; // 3DS
            case 0x59: texture_format = DDS_FORMAT_DXT1; break; // Win
            case 0x5A: texture_format = DDS_FORMAT_DXT3; break; // Win
            case 0x5B: texture_format = DDS_FORMAT_DXT5; break; // Win
            case 0x5C: texture_format = DDS_FORMAT_BC4; break;  // Win
//            case 0x5D: texture_format = DDS_FORMAT_ATI1; break;
            case 0x5E: texture_format = DDS_FORMAT_BC6H; break; // Win
            case 0x5F: texture_format = DDS_FORMAT_BC7; break;  // Win
            case 0x60: texture_format = DDS_FORMAT_DXT1; swizzled = true; break;    // PS4
            case 0x61: texture_format = DDS_FORMAT_DXT3; swizzled = true; break;    // PS4
            case 0x62: texture_format = DDS_FORMAT_DXT5; swizzled = true; break;    // PS4
//            case 0x63: texture_format = DDS_FORMAT_BC4; swizzled = true; break;
//            case 0x64: texture_format = DDS_FORMAT_BC5; swizzled = true; break;
//            case 0x65: texture_format = DDS_FORMAT_BC6; swizzled = true; break;
//            case 0x66: texture_format = DDS_FORMAT_BC7; swizzled = true; break;
            case 0x72: texture_format = DDS_FORMAT_BC7; break;   // Win
            default:
                fprintf(stderr, "ERROR: Unsupported texture type 0x%02x\n", tex.type);
                goto out;
            }

            uint32_t expected_texture_size = 0;
            uint32_t min_mipmap_size = dds_bpb(texture_format);
            if (hdr.platform == NINTENDO_WIIU)
                min_mipmap_size = 0x40 * dds_bpb(texture_format);
            for (int j = 0; j < tex.mipmaps; j++)
                expected_texture_size += MIPMAP_SIZE(texture_format, j, dds_header->width, dds_header->height);
            expected_texture_size *= nb_frames;
            bool cubemap = dds_header->caps & DDS_SURFACE_FLAGS_CUBEMAP && dds_header->caps2 & DDS_CUBEMAP_ALLFACES;
            if (cubemap) {
                if ((dds_header->caps2 & DDS_CUBEMAP_ALLFACES) != DDS_CUBEMAP_ALLFACES) {
                    fprintf(stderr, "ERROR: Cannot handle cube maps with missing faces\n");
                    goto out;
                }
                expected_texture_size *= 6;
            }
            if (expected_texture_size > texture_size) {
                fprintf(stderr, "ERROR: expected_texture_size %8x > %8x\n", expected_texture_size, texture_size);
                goto out;
            }
            if ((texture_size * 8) % dds_bpp(texture_format) != 0) {
                fprintf(stderr, "ERROR: Texture size should be a multiple of %d bits\n", dds_bpp(texture_format));
                goto out;
            }
            if (expected_texture_size < texture_size) {
                // Only display the warning if we aren't truncating mipmaps
                if ((uint8_t)dds_header->mipMapCount <= tex.mipmaps)
                    fprintf(stderr, "WARNING: Reducing texture size\n");
                texture_size = expected_texture_size;
            }

            switch (dds_header->ddspf.flags & (DDS_ALPHAPIXELS | DDS_FOURCC | DDS_RGB)) {
            case DDS_RGBA:
                if ((dds_header->ddspf.RGBBitCount != 16) && (dds_header->ddspf.RGBBitCount != 32) &&
                    (dds_header->ddspf.RGBBitCount != 64) && (dds_header->ddspf.RGBBitCount != 128)) {
                    fprintf(stderr, "ERROR: '%s' is not an ARGB texture we support\n", path);
                    goto out;
                }
                break;
            case DDS_RGB:
                if ((dds_header->ddspf.RGBBitCount != 24) ||
                    (dds_header->ddspf.RBitMask != 0x00ff0000) || (dds_header->ddspf.GBitMask != 0x0000ff00) ||
                    (dds_header->ddspf.BBitMask != 0x000000ff) || (dds_header->ddspf.ABitMask != 0x00000000)) {
                    fprintf(stderr, "ERROR: '%s' is not an RGB texture we support\n", path);
                    goto out;
                }
            case DDS_FOURCC:
                break;
            default:
                fprintf(stderr, "ERROR: '%s' is not a texture we support\n", path);
                goto out;
            }

            if (flip_image || ((hdr.platform == NINTENDO_3DS) && (tex.type == 0x09 || tex.type == 0x45)))
                flip(dds_bpp(texture_format), dds_payload, texture_size, dds_header->width);

            if (swizzled) {
                int16_t mo = 0;     // Morton order
                uint32_t wf = 1;    // Width factor
                uint32_t awf = 1;   // Additional width factor
                switch (hdr.platform) {
                case SONY_PS4:
                case NINTENDO_3DS:
                    mo = 3;
                    wf = 2;
                    break;
                case NINTENDO_WIIU:
                    mo = 1;        // Same for all mipmaps
                    wf = 16 / dds_bpb(texture_format);
                    awf = 8;
                    break;
                default:
                    mo = (int16_t)log2(min(dds_header->width / dds_bwh(texture_format) / wf,
                            dds_header->height / dds_bwh(texture_format)));
                    break;
                }
                uint32_t offset = 0;
                assert(mo != 0);
                // TODO: We'll need to handle morton for texture arrays & cubemaps
                for (int j = 0; j < tex.mipmaps && mo != 0; j++) {
                    uint32_t mipmap_size = MIPMAP_SIZE(texture_format, j, dds_header->width, dds_header->height);
                    uint32_t mw = max(awf * dds_bwh(texture_format), dds_header->width / (1 << j));
                    uint32_t mh = max(awf * dds_bwh(texture_format), dds_header->height / (1 << j));
                    uint8_t* mipmap_buf = calloc(max(mipmap_size, min_mipmap_size), 1);
                    if (mipmap_buf == NULL)
                        goto out;
                    memcpy(mipmap_buf, &dds_payload[offset], mipmap_size);
                    if (dds_header->width / (1 << j) < mw)
                        untile(texture_format, dds_header->width / (1 << j), mw, mipmap_buf, max(min_mipmap_size, mipmap_size));
                    mortonize(texture_format, mo, mw, mh, mipmap_buf, max(mipmap_size, min_mipmap_size), wf);
                    memcpy(&dds_payload[offset], mipmap_buf, mipmap_size);
                    free(mipmap_buf);
                    offset += mipmap_size;
                    if (hdr.platform != NINTENDO_WIIU)
                        mo += (mo > 0) ? -1 : +1;
                }
            }
            if (texture_format >= DDS_FORMAT_ABGR4 && texture_format <= DDS_FORMAT_RGBA8)
                rgba_convert(texture_format, "ARGB", argb_name[texture_format], dds_payload, texture_size);

            char dims[16] = { 0 }, props[8] = { 0 };
            snprintf(dims, sizeof(dims), "%dx%d", dds_header->width, dds_header->height);
            if (nb_frames > 1)
                strcat(props, "A");     // Array
            if (data_endianness == big_endian)
                strcat(props, "B");
            if (cubemap)
                strcat(props, "C");     // Cubemap
            if (depth != 0.0f)
                strcat(props, "D");
            if (props[0] == 0)
                props[0] = '-';
            printf("0x%02x 0x%08x 0x%08x %s %-10s %-7d %s\n", tex.type, getv32(hdr.header_size) + offset_table[i],
                (uint32_t)ftell(file) - offset_table[i] - getv32(hdr.header_size) - (uint32_t)sizeof(g1t_tex_header),
                path, dims, tex.mipmaps, props);

            if (cubemap)
                nb_frames *= 6;     // Adjust effective nb_frames for cubemaps
            // Inverse operation from the one we carry when extracting DDS
            uint32_t f_size = texture_size / nb_frames;
            for (uint32_t l = 0, offset = 0; l < tex.mipmaps; l++) {
                uint32_t mipmap_size = MIPMAP_SIZE(texture_format, l, dds_header->width, dds_header->height);
                for (uint32_t f = 0; f < nb_frames; f++) {
                    if (fwrite(&dds_payload[f * f_size + offset], mipmap_size, 1, file) != 1) {
                        fprintf(stderr, "ERROR: Can't write DDS data\n");
                        goto out;
                    }
                    if (mipmap_size < min_mipmap_size) {
                        uint8_t* completion_buf = calloc(min_mipmap_size - mipmap_size, 1);
                        if (completion_buf == NULL)
                            goto out;
                        if (fwrite(completion_buf, min_mipmap_size - mipmap_size, 1, file) != 1) {
                            fprintf(stderr, "ERROR: Can't write DDS data\n");
                            goto out;
                        }
                        free(completion_buf);
                    }
                }
                offset += mipmap_size;
            }
            free(buf);
            buf = NULL;
        }
        // Update total size
        uint32_t total_size = getv32(ftell(file));
        fseek(file, 2 * sizeof(uint32_t), SEEK_SET);
        if (fwrite(&total_size, sizeof(uint32_t), 1, file) != 1) {
            fprintf(stderr, "ERROR: Can't update total size\n");
            goto out;
        }
        // Update flag and offset tables
        fseek(file, sizeof(hdr), SEEK_SET);
        fix_endian32(flag_table, hdr.nb_textures);
        if (fwrite(flag_table, sizeof(uint32_t), hdr.nb_textures, file) != hdr.nb_textures) {
            fprintf(stderr, "ERROR: Can't update global flags\n");
            goto out;
        }
        fix_endian32(offset_table, hdr.nb_textures);
        if (fwrite(offset_table, sizeof(uint32_t), hdr.nb_textures, file) != hdr.nb_textures) {
            fprintf(stderr, "ERROR: Can't update texture offsets\n");
            goto out;
        }
        r = 0;
    } else {
        printf("%s '%s'...\n", list_only ? "Listing" : "Extracting", argv[argc - 1]);
        size_t len = strlen(argv[argc - 1]);
        if ((len < 4) || (argv[argc - 1][len - 4] != '.') || (argv[argc - 1][len - 3] != 'g') ||
            ((argv[argc - 1][len - 2] != '1') && (argv[argc - 1][len - 2] != 't')) ||
            ((argv[argc - 1][len - 1] != '1') && (argv[argc - 1][len - 1] != 't')) ) {
            fprintf(stderr, "ERROR: File should have a '.g1t' or 'gt1' extension\n");
            goto out;
        }
        char* g1t_pos = &argv[argc - 1][len - 4];
        file = fopen_utf8(argv[argc - 1], "rb");
        if (file == NULL) {
            fprintf(stderr, "ERROR: Can't open file '%s'\n", argv[argc - 1]);
            goto out;
        }

        if (fread(&magic, sizeof(magic), 1, file) != 1) {
            fprintf(stderr, "ERROR: Can't read from '%s'\n", argv[argc - 1]);
            goto out;
        }
        if ((magic != G1TG_MAGIC) && (magic != bswap_uint32(G1TG_MAGIC))) {
            fprintf(stderr, "ERROR: Not a G1T file (bad magic) or unsupported platform\n");
            goto out;
        }
        if (magic == bswap_uint32(G1TG_MAGIC))
            data_endianness = !platform_endianness;
        fseek(file, 0L, SEEK_END);
        uint32_t g1t_size = (uint32_t)ftell(file);
        fseek(file, 0L, SEEK_SET);

        buf = malloc(g1t_size);
        if (buf == NULL)
            goto out;
        if (fread(buf, 1, g1t_size, file) != g1t_size) {
            fprintf(stderr, "ERROR: Can't read file\n");
            goto out;
        }

        g1t_header* hdr = (g1t_header*)buf;
        fix_endian32(hdr, sizeof(g1t_header) / sizeof(uint32_t));
        if (hdr->total_size != g1t_size) {
            fprintf(stderr, "ERROR: File size mismatch\n");
            goto out;
        }
        char version_string[5] = { 0 };
        setbe32(version_string, hdr->version);
        version_string[4] = 0;
        if (hdr->version >> 16 != 0x3030 && hdr->version >> 16 != 0x3031)
            fprintf(stderr, "WARNING: Potentially unsupported G1T version %s\n", version_string);
        int version = atoi(version_string);
        if (version == 0 || version > 10000) {
            fprintf(stderr, "ERROR: Unexpected G1T version %s\n", version_string);
            goto out;
        }
        if (hdr->extra_size % sizeof(uint32_t)) {
            fprintf(stderr, "ERROR: Can't handle G1T files with global extra data that's not a multiple of %d\n",
                (int)sizeof(uint32_t));
            goto out;
        }
        if (hdr->extra_size > 0xFFFF) {
            fprintf(stderr, "ERROR: Can't handle G1T files with more than 64 KB of global extra data\n");
            goto out;
        }

        uint32_t* x_offset_table = (uint32_t*)&buf[hdr->header_size];

        // Keep the information required to recreate the archive in a JSON file
        json = json_value_init_object();
        json_object_set_number(json_object(json), "json_version", JSON_VERSION);
        json_object_set_string(json_object(json), "name", _basename(argv[argc - 1]));
        json_object_set_number(json_object(json), "version", version);
        if (platform_to_name(hdr->platform) != NULL)
            json_object_set_string(json_object(json), "platform", platform_to_name(hdr->platform));
        else
            json_object_set_number(json_object(json), "platform", hdr->platform);
        if (flip_image)
            json_object_set_boolean(json_object(json), "flip", true);

        g1t_pos[0] = 0;
        if (!list_only && !create_path(argv[argc - 1]))
            goto out;

        JSON_Value* json_extra_data_array = json_value_init_array();
        JSON_Value* json_textures_array = json_value_init_array();

        for (uint16_t i = 0; i < hdr->extra_size; i += sizeof(uint16_t))
            json_array_append_number(json_array(json_extra_data_array),
                getp16(&buf[hdr->header_size + hdr->nb_textures * sizeof(uint32_t) + i]));

        printf("TYPE OFFSET     SIZE       NAME");
        for (size_t i = 0; i < strlen(_basename(argv[argc - 1])); i++)
            putchar(' ');
        printf("     DIMENSIONS MIPMAPS PROPS\n");
        dir = strdup(argv[argc - 1]);
        if (dir == NULL) {
            fprintf(stderr, "ERROR: Alloc error\n");
            goto out;
        }
        dir[get_trailing_slash(dir)] = 0;

        // Set the default RGBA texture format for the platform
        uint32_t default_texture_format;
        switch (hdr->platform) {
        case NINTENDO_DS:
        case NINTENDO_3DS:
        case SONY_PS4:
            default_texture_format = DDS_FORMAT_GRAB8;
            break;
        case SONY_PSV:
        case NINTENDO_SWITCH:
            default_texture_format = DDS_FORMAT_ARGB8;
            break;
        default:    // PC and other platforms
            default_texture_format = DDS_FORMAT_RGBA8;
            break;
        }

        uint32_t i;
        for (i = 0; i < hdr->nb_textures; i++) {
            uint32_t nb_frames = 0, pos = hdr->header_size + getv32(x_offset_table[i]);
            g1t_tex_header* tex = (g1t_tex_header*)&buf[pos];
            float depth = 0.0f;
            if (data_endianness == big_endian) {
                uint8_t swap_tmp = tex->dx;
                tex->dx = tex->dy;
                tex->dy = swap_tmp;
                swap_tmp = tex->z_mipmaps;
                tex->z_mipmaps = tex->mipmaps;
                tex->mipmaps = swap_tmp;
            } else {
                for (size_t j = 0; j < array_size(tex->flags); j++)
                    tex->flags[j] = tex->flags[j] >> 4 | tex->flags[j] << 4;
            }
            if (tex->mipmaps == 0) {
                fprintf(stderr, "ERROR: Number of mipmaps is 0\n");
                fprintf(stderr, "Please report this error to %s.\n", REPORT_URL);
                break;
            }
            // We're going to assume that the global flags (the ones after the G1T global header)
            // never see a value higher than 0x00ffffff, so that we can concatenate all the main
            // texture flags together. We're also going to assume that the 4 bytes in the extra
            // data (after the extra data size and the depth) are additionnal flags in big-endian.
            uint64_t flags[2] = { 0 };
            flags[0] = (uint64_t)getp32(&buf[(uint32_t)sizeof(g1t_header) + 4 * i]);
            if (flags[0] & 0xff000000ULL) {
                fprintf(stderr, "ERROR: Global flags 0x%08x don't match our assertion\n", (uint32_t)flags[0]);
                fprintf(stderr, "Please report this error to %s.\n", REPORT_URL);
                break;
            }
            for (size_t j = 0; j < array_size(tex->flags); j++)
                flags[0] = flags[0] << 8 | (uint64_t)tex->flags[j];
            pos += sizeof(g1t_tex_header);
            uint32_t width = 1 << tex->dx;
            uint32_t height = 1 << tex->dy;
            if (flags[0] & G1T_FLAG_DOUBLE_HEIGHT)
                height *= 2;
            uint32_t data_size = (flags[0] & G1T_FLAG_EXTENDED_DATA) ? getp32(&buf[pos]) : 0;
            if (data_size != 0 && data_size != 0x0c && data_size != 0x10 && data_size != 0x14) {
                fprintf(stderr, "ERROR: Extra flags size of 0x%x doesn't match our assertion\n", data_size);
                fprintf(stderr, "Please report this error to %s.\n", REPORT_URL);
                break;
            }
            // Extra flags, including the number of frames, may be provided
            if (data_size >= 0x0c) {
                uint32_t _depth = getp32(&buf[pos + 4]);
                depth = *((float*)&_depth);
                flags[1] = getbe32(&buf[pos + 8]);
                nb_frames = GET_NB_FRAMES(flags[1]);
            }
            if (nb_frames == 0)
                nb_frames = 1;
            // Non power-of-two width and height may be provided in the data
            if (data_size >= 0x10)
                width = getp32(&buf[pos + 0x0c]);
            if (data_size >= 0x14)
                height = getp32(&buf[pos + 0x10]);

            JSON_Value* json_texture = json_value_init_object();
            snprintf(path, sizeof(path), "%03d.dds", i);
            json_object_set_string(json_object(json_texture), "name", path);
            json_object_set_number(json_object(json_texture), "type", tex->type);
            if (tex->mipmaps != 1)
                json_object_set_number(json_object(json_texture), "mipmaps", tex->mipmaps);
            if (tex->z_mipmaps != 0)
                json_object_set_number(json_object(json_texture), "z_mipmaps", tex->z_mipmaps);
            if (nb_frames > 1)
                json_object_set_number(json_object(json_texture), "nb_frames", nb_frames);
            if (depth != 0.0f) {
                char depth_str[16];
                snprintf(depth_str, sizeof(depth_str), "%f", depth);
                json_object_set_string(json_object(json_texture), "depth", depth_str);
            }
            uint32_t texture_format = default_texture_format;
            bool swizzled = false;
            switch (tex->type) {
            case 0x00: break;
            case 0x01: break;
            case 0x02: break;
            case 0x03: texture_format = DDS_FORMAT_ARGB16; break;
            case 0x04: texture_format = DDS_FORMAT_ARGB32; break;
            case 0x06: texture_format = DDS_FORMAT_DXT1; break;
//            case 0x07: texture_format = DDS_FORMAT_DXT3; break;
            case 0x08: texture_format = DDS_FORMAT_DXT5; break;
            case 0x09: swizzled = true; break;
//            case 0x0A: swizzled = true; break;
            case 0x10: texture_format = DDS_FORMAT_DXT1; swizzled = true; break;
            case 0x12: texture_format = DDS_FORMAT_DXT5; swizzled = true; break;
            case 0x21: break;
            case 0x3C: texture_format = DDS_FORMAT_ARGB4; break;
            case 0x3D: texture_format = DDS_FORMAT_ARGB4; break;
            case 0x45: texture_format = DDS_FORMAT_BGR8; swizzled = true; break;
            case 0x59: texture_format = DDS_FORMAT_DXT1; break;
            case 0x5B: texture_format = DDS_FORMAT_DXT5; break;
            case 0x5C: texture_format = DDS_FORMAT_BC4; break;
//            case 0x5D: texture_format = DDS_FORMAT_ATI1; break;
            case 0x5E: texture_format = DDS_FORMAT_BC6H; break;
            case 0x5F: texture_format = DDS_FORMAT_BC7; break;
            case 0x60: texture_format = DDS_FORMAT_DXT1; swizzled = true; break;
            case 0x62: texture_format = DDS_FORMAT_DXT5; swizzled = true; break;
//            case 0x63: texture_format = DDS_FORMAT_BC4; swizzled = true; break;
//            case 0x64: texture_format = DDS_FORMAT_BC5; swizzled = true; break;
//            case 0x65: texture_format = DDS_FORMAT_BC6; swizzled = true; break;
//            case 0x66: texture_format = DDS_FORMAT_BC7; swizzled = true; break;
            // 0x72 is not actually BC7, but that's the closest we get to semi-recognizable output
            case 0x72: texture_format = DDS_FORMAT_BC7; break;
            default:
                fprintf(stderr, "ERROR: Unsupported texture type (0x%02X)\n", tex->type);
                fprintf(stderr, "Please visit: https://github.com/VitaSmith/gust_tools/issues/51\n");
                goto out;
            }
            uint32_t expected_texture_size = 0;
            uint32_t min_mipmap_size = dds_bpb(texture_format);
            if (hdr->platform == NINTENDO_WIIU)
                min_mipmap_size = 0x40 * dds_bpb(texture_format);
            for (int j = 0; j < tex->mipmaps; j++)
                expected_texture_size += nb_frames * max(MIPMAP_SIZE(texture_format, j, width, height), min_mipmap_size);
            uint32_t texture_size = ((i + 1 == hdr->nb_textures) ?
                g1t_size - hdr->header_size : getv32(x_offset_table[i + 1])) - getv32(x_offset_table[i]);
            texture_size -= (uint32_t)sizeof(g1t_tex_header);
            if (flags[0] & G1T_FLAG_EXTENDED_DATA) {
                assert(pos + data_size < g1t_size);
                if ((data_size != 0x0c) && (data_size != 0x10) && (data_size != 0x14)) {
                    fprintf(stderr, "ERROR: Can't handle local extra_data of size 0x%08x\n", data_size);
                    break;
                }
                pos += data_size;
                texture_size -= data_size;
            }
            if (texture_size < expected_texture_size) {
                fprintf(stderr, "ERROR: Actual texture size is smaller than expected size\n");
                break;
            } else if (texture_size > expected_texture_size) {
                if (texture_size % expected_texture_size != 0) {
                    fprintf(stderr, "WARNING: Actual texture size is larger than expected size by 0x%x\n",
                        texture_size - expected_texture_size);
                } else if (texture_size / expected_texture_size == 6) {
                    // A cubemap is composed of one texture for each face
                    flags[1] |= G1T_FLAG_CUBE_MAP;
                } else {
                    fprintf(stderr, "ERROR: Cube map with a factor of %d doesn't match our assertion\n",
                        texture_size / expected_texture_size);
                    fprintf(stderr, "Please report this error to %s.\n", REPORT_URL);
                    break;
                }
                expected_texture_size = texture_size;
            }
            json_object_set_value(json_object(json_texture), "flags", flags_to_json(flags));

            snprintf(path, sizeof(path), "%s%s%c%03d.dds", dir, _basename(argv[argc - 1]), PATH_SEP, i);
            char dims[16] = { 0 }, props[8] = { 0 };
            snprintf(dims, sizeof(dims), "%dx%d", width, height);
            if (flags[1] & G1T_FLAG_TEXTURE_ARRAY)
                strcat(props, "A");
            if (data_endianness == big_endian)
                strcat(props, "B");
            if (flags[1] & G1T_FLAG_CUBE_MAP)
                strcat(props, "C");
            if (depth != 0)
                strcat(props, "D");
            if (props[0] == 0)
                props[0] = '-';
            printf("0x%02x 0x%08x 0x%08x %s %-10s %-7d %s\n", tex->type,
                hdr->header_size + hdr->extra_size + getv32(x_offset_table[i]),
                texture_size, &path[strlen(dir)], dims, tex->mipmaps, props);
            if (list_only) {
                json_value_free(json_texture);
                continue;
            }
            FILE* dst = fopen_utf8(path, "wb");
            if (dst == NULL) {
                fprintf(stderr, "ERROR: Can't create file '%s'\n", path);
                break;
            }
            uint32_t dds_magic = DDS_MAGIC;
            if (fwrite(&dds_magic, sizeof(dds_magic), 1, dst) != 1) {
                fprintf(stderr, "ERROR: Can't write magic\n");
                fclose(dst);
                r = -1;
                break;
            }
            if (write_dds_header(dst, texture_format, width, height,
                                 tex->mipmaps, flags) != 1) {
                fprintf(stderr, "ERROR: Can't write DDS header\n");
                fclose(dst);
                break;
            }

            // Non ARGB textures require conversion to be applied, since
            // tools like Visual Studio or PhotoShop can't be bothered
            // to honour the pixel format from the DDS header and instead
            // insist on using ARGB always...
            if (texture_format >= DDS_FORMAT_ABGR4 && texture_format <= DDS_FORMAT_RGBA8)
                rgba_convert(texture_format, argb_name[texture_format], "ARGB", &buf[pos], expected_texture_size);
            if (swizzled) {
                int16_t mo = 0;     // Morton order
                uint32_t wf = 1;    // Width factor
                uint32_t awf = 1;   // Additional width factor
                switch (hdr->platform) {
                case SONY_PS4:
                case NINTENDO_3DS:
                    mo = -3;
                    wf = 2;
                    break;
                case NINTENDO_WIIU:
                    mo = -1;        // Same for all mipmaps
                    wf = 16 / dds_bpb(texture_format);
                    awf = 8;
                    break;
                default:
                    mo = -1 * (int16_t)log2(min(width / dds_bwh(texture_format) / wf,
                        height / dds_bwh(texture_format)));
                    break;
                }
                uint32_t offset = 0;
                assert(mo != 0);
                for (int j = 0; j < tex->mipmaps && mo != 0; j++) {
                    uint32_t mipmap_size = max(MIPMAP_SIZE(texture_format, j, width, height), min_mipmap_size);
                    uint32_t mw = max(awf * dds_bwh(texture_format), width / (1 << j));
                    uint32_t mh = max(awf * dds_bwh(texture_format), height / (1 << j));
                    mortonize(texture_format, mo, mw, mh, &buf[pos + offset], mipmap_size, wf);
                    if (width / (1 << j) < mw)
                        tile(texture_format, width / (1 << j), mw, &buf[pos + offset], mipmap_size);
                    offset += mipmap_size;
                    if (hdr->platform != NINTENDO_WIIU)
                        mo += (mo > 0) ? -1 : +1;
                }
            }
            if (flip_image || ((hdr->platform == NINTENDO_3DS) && (tex->type == 0x09 || tex->type == 0x45)))
                flip(dds_bpp(texture_format), &buf[pos], expected_texture_size, width);
            // DDS expects the mipmaps of a texture array or cubemap to immediately follow
            // the main one, but G1T instead stores all mains, then all L1 mipmaps, then
            // all L2 mipmaps and so on... Thus we need to manually reorder the mipmaps.
            if (flags[1] & G1T_FLAG_CUBE_MAP)
                nb_frames *= 6;     // Adjust effective nb_frames for cubemaps
            for (uint32_t f = 0; f < nb_frames; f++) {
                for (uint32_t l = 0, offset = 0; l < tex->mipmaps; l++) {
                    uint32_t mipmap_size = max(MIPMAP_SIZE(texture_format, l, width, height), min_mipmap_size);
                    offset += f * mipmap_size;
                    if (fwrite(&buf[pos + offset], MIPMAP_SIZE(texture_format, l, width, height), 1, dst) != 1) {
                        fprintf(stderr, "ERROR: Can't write DDS data\n");
                        goto out;
                    }
                    offset += (nb_frames - f) * mipmap_size;
                }
            }
            fclose(dst);
            json_array_append_value(json_array(json_textures_array), json_texture);
        }
        r = (i == hdr->nb_textures) ? 0 : -1;

        json_object_set_value(json_object(json), "textures", json_textures_array);
        if (hdr->extra_size)
            json_object_set_value(json_object(json), "extra_data", json_extra_data_array);
        else
            json_value_free(json_extra_data_array);
        snprintf(path, sizeof(path), "%s%cg1t.json", argv[argc - 1], PATH_SEP);
        if (!list_only)
            json_serialize_to_file_pretty(json, path);
    }

out:
    json_value_free(json);
    free(buf);
    free(dir);
    free(offset_table);
    free(flag_table);
    if (file != NULL)
        fclose(file);

    if (r != 0 && !no_prompt) {
        fflush(stdin);
        printf("\nPress any key to continue...");
        (void)getchar();
    }

    return r;
}

CALL_MAIN

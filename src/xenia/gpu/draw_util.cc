/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/draw_util.h"

#include <cmath>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/xenos.h"

DEFINE_bool(
    resolve_resolution_scale_duplicate_second_pixel, true,
    "When using resolution scale, apply the hack that duplicates the "
    "right/lower host pixel in the left and top sides of render target resolve "
    "areas to eliminate the gap caused by half-pixel offset (this is necessary "
    "for certain games like GTA IV to work).",
    "GPU");

namespace xe {
namespace gpu {
namespace draw_util {

int32_t FloatToD3D11Fixed16p8(float f32) {
  // https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#3.2.4.1%20FLOAT%20-%3E%20Fixed%20Point%20Integer
  // Early exit tests.
  // n == NaN || n.unbiasedExponent < -f-1 -> 0 . 0
  if (!(std::abs(f32) >= 1.0f / 512.0f)) {
    return 0;
  }
  // n >= (2^(i-1)-2^-f) -> 2^(i-1)-1 . 2^f-1
  if (f32 >= 32768.0f - 1.0f / 256.0f) {
    return (1 << 23) - 1;
  }
  // n <= -2^(i-1) -> -2^(i-1) . 0
  if (f32 <= -32768.0f) {
    return -32768 * 256;
  }
  uint32_t f32_bits = *reinterpret_cast<const uint32_t*>(&f32);
  // Copy float32 mantissa bits [22:0] into corresponding bits [22:0] of a
  // result buffer that has at least 24 bits total storage (before reaching
  // rounding step further below). This includes one bit for the hidden 1.
  // Set bit [23] (float32 hidden bit).
  // Clear bits [31:24].
  union {
    int32_t s;
    uint32_t u;
  } result;
  result.u = (f32_bits & ((1 << 23) - 1)) | (1 << 23);
  // If the sign bit is set in the float32 number (negative), then take the 2's
  // component of the entire set of bits.
  if ((f32_bits >> 31) != 0) {
    result.s = -result.s;
  }
  // Final calculation: extraBits = (mantissa - f) - n.unbiasedExponent
  // (guaranteed to be >= 0).
  int32_t exponent = int32_t((f32_bits >> 23) & 255) - 127;
  uint32_t extra_bits = uint32_t(15 - exponent);
  if (extra_bits) {
    // Round the 32-bit value to a decimal that is extraBits to the left of
    // the LSB end, using nearest-even.
    result.u += (1 << (extra_bits - 1)) - 1 + ((result.u >> extra_bits) & 1);
    // Shift right by extraBits (sign extending).
    result.s >>= extra_bits;
  }
  return result.s;
}

xenos::CopySampleSelect SanitizeCopySampleSelect(
    xenos::CopySampleSelect copy_sample_select, xenos::MsaaSamples msaa_samples,
    bool is_depth) {
  // Depth can't be averaged.
  if (msaa_samples >= xenos::MsaaSamples::k4X) {
    if (copy_sample_select > xenos::CopySampleSelect::k0123) {
      copy_sample_select = xenos::CopySampleSelect::k0123;
    }
    if (is_depth) {
      switch (copy_sample_select) {
        case xenos::CopySampleSelect::k01:
        case xenos::CopySampleSelect::k0123:
          copy_sample_select = xenos::CopySampleSelect::k0;
          break;
        case xenos::CopySampleSelect::k23:
          copy_sample_select = xenos::CopySampleSelect::k2;
          break;
        default:
          break;
      }
    }
  } else if (msaa_samples >= xenos::MsaaSamples::k2X) {
    switch (copy_sample_select) {
      case xenos::CopySampleSelect::k2:
        copy_sample_select = xenos::CopySampleSelect::k0;
        break;
      case xenos::CopySampleSelect::k3:
        copy_sample_select = xenos::CopySampleSelect::k1;
        break;
      default:
        if (copy_sample_select > xenos::CopySampleSelect::k01) {
          copy_sample_select = xenos::CopySampleSelect::k01;
        }
    }
    if (is_depth && copy_sample_select == xenos::CopySampleSelect::k01) {
      copy_sample_select = xenos::CopySampleSelect::k0;
    }
  } else {
    copy_sample_select = xenos::CopySampleSelect::k0;
  }
  return copy_sample_select;
}

const ResolveCopyShaderInfo
    resolve_copy_shader_info[size_t(ResolveCopyShaderIndex::kCount)] = {
        {"Resolve Copy Fast 32bpp 1x/2xMSAA", 1, false, 4, 4, 6, 3},
        {"Resolve Copy Fast 32bpp 4xMSAA", 1, false, 4, 4, 6, 3},
        {"Resolve Copy Fast 32bpp 2xRes", 2, false, 4, 4, 4, 3},
        {"Resolve Copy Fast 64bpp 1x/2xMSAA", 1, false, 4, 4, 5, 3},
        {"Resolve Copy Fast 64bpp 4xMSAA", 1, false, 3, 4, 5, 3},
        {"Resolve Copy Fast 64bpp 2xRes", 2, false, 4, 4, 3, 3},
        {"Resolve Copy Full 8bpp", 1, true, 2, 3, 6, 3},
        {"Resolve Copy Full 8bpp 2xRes", 2, false, 4, 3, 4, 3},
        {"Resolve Copy Full 16bpp", 1, true, 2, 3, 5, 3},
        {"Resolve Copy Full 16bpp 2xRes", 2, false, 4, 3, 3, 3},
        {"Resolve Copy Full 32bpp", 1, true, 2, 4, 5, 3},
        {"Resolve Copy Full 32bpp 2xRes", 2, false, 4, 4, 3, 3},
        {"Resolve Copy Full 64bpp", 1, true, 2, 4, 5, 3},
        {"Resolve Copy Full 64bpp 2xRes", 2, false, 4, 4, 3, 3},
        {"Resolve Copy Full 128bpp", 1, true, 2, 4, 4, 3},
        {"Resolve Copy Full 128bpp 2xRes", 2, false, 4, 4, 3, 3},
};

bool GetResolveInfo(const RegisterFile& regs, const Memory& memory,
                    TraceWriter& trace_writer, uint32_t resolution_scale,
                    bool edram_16_as_minus_1_to_1, ResolveInfo& info_out) {
  auto rb_copy_control = regs.Get<reg::RB_COPY_CONTROL>();
  info_out.rb_copy_control = rb_copy_control;

  if (rb_copy_control.copy_command != xenos::CopyCommand::kRaw &&
      rb_copy_control.copy_command != xenos::CopyCommand::kConvert) {
    XELOGE(
        "Unsupported resolve copy command {}. Report the game to Xenia "
        "developers",
        uint32_t(rb_copy_control.copy_command));
    assert_always();
    return false;
  }

  // Don't pass uninitialized values to shaders, not to leak data to frame
  // captures.
  info_out.address.packed = 0;

  // Get the extent of pixels covered by the resolve rectangle, according to the
  // top-left rasterization rule.
  // D3D9 HACK: Vertices to use are always in vf0, and are written by the CPU.
  auto fetch = regs.Get<xenos::xe_gpu_vertex_fetch_t>(
      XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0);
  if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 3 * 2) {
    XELOGE("Unsupported resolve vertex buffer format");
    assert_always();
    return false;
  }
  trace_writer.WriteMemoryRead(fetch.address * sizeof(uint32_t),
                               fetch.size * sizeof(uint32_t));
  const float* vertices_guest = reinterpret_cast<const float*>(
      memory.TranslatePhysical(fetch.address * sizeof(uint32_t)));
  // Most vertices have a negative half-pixel offset applied, which we reverse.
  float half_pixel_offset =
      regs.Get<reg::PA_SU_VTX_CNTL>().pix_center ? 0.0f : 0.5f;
  int32_t vertices_fixed[6];
  for (size_t i = 0; i < xe::countof(vertices_fixed); ++i) {
    vertices_fixed[i] = FloatToD3D11Fixed16p8(
        xenos::GpuSwap(vertices_guest[i], fetch.endian) + half_pixel_offset);
  }
  // Inclusive.
  int32_t x0 = std::min(std::min(vertices_fixed[0], vertices_fixed[2]),
                        vertices_fixed[4]);
  int32_t y0 = std::min(std::min(vertices_fixed[1], vertices_fixed[3]),
                        vertices_fixed[5]);
  // Exclusive.
  int32_t x1 = std::max(std::max(vertices_fixed[0], vertices_fixed[2]),
                        vertices_fixed[4]);
  int32_t y1 = std::max(std::max(vertices_fixed[1], vertices_fixed[3]),
                        vertices_fixed[5]);
  // Top-left - include .5 (0.128 treated as 0 covered, 0.129 as 0 not covered).
  x0 = (x0 + 127) >> 8;
  y0 = (y0 + 127) >> 8;
  // Bottom-right - exclude .5.
  x1 = (x1 + 127) >> 8;
  y1 = (y1 + 127) >> 8;

  auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();

  // Apply the window offset to the vertices.
  if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    x0 += pa_sc_window_offset.window_x_offset;
    y0 += pa_sc_window_offset.window_y_offset;
    x1 += pa_sc_window_offset.window_x_offset;
    y1 += pa_sc_window_offset.window_y_offset;
  }

  // Apply the scissor and prevent negative origin (behind the EDRAM base).
  auto pa_sc_window_scissor_tl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
  auto pa_sc_window_scissor_br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  int32_t scissor_x0 = int32_t(pa_sc_window_scissor_tl.tl_x);
  int32_t scissor_y0 = int32_t(pa_sc_window_scissor_tl.tl_y);
  int32_t scissor_x1 =
      std::max(int32_t(pa_sc_window_scissor_br.br_x), scissor_x0);
  int32_t scissor_y1 =
      std::max(int32_t(pa_sc_window_scissor_br.br_y), scissor_y0);
  if (!pa_sc_window_scissor_tl.window_offset_disable) {
    scissor_x0 =
        std::max(scissor_x0 + pa_sc_window_offset.window_x_offset, int32_t(0));
    scissor_y0 =
        std::max(scissor_y0 + pa_sc_window_offset.window_y_offset, int32_t(0));
    scissor_x1 =
        std::max(scissor_x1 + pa_sc_window_offset.window_x_offset, int32_t(0));
    scissor_y1 =
        std::max(scissor_y1 + pa_sc_window_offset.window_y_offset, int32_t(0));
  }
  x0 = xe::clamp(x0, scissor_x0, scissor_x1);
  y0 = xe::clamp(y0, scissor_y0, scissor_y1);
  x1 = xe::clamp(x1, scissor_x0, scissor_x1);
  y1 = xe::clamp(y1, scissor_y0, scissor_y1);

  assert_true(x0 <= x1 && y0 <= y1);

  // Direct3D 9's D3DDevice_Resolve internally rounds the right/bottom of the
  // rectangle internally to 8. While all the alignment should have already been
  // done by Direct3D 9, just for safety of host implementation of resolve,
  // force-align the rectangle by expanding (D3D9 expands to the right/bottom
  // for some reason, haven't found how left/top is rounded, but logically it
  // would make sense to expand to the left/top too).
  x0 &= ~int32_t(xenos::kResolveAlignmentPixels - 1);
  y0 &= ~int32_t(xenos::kResolveAlignmentPixels - 1);
  x1 = xe::align(x1, int32_t(xenos::kResolveAlignmentPixels));
  y1 = xe::align(y1, int32_t(xenos::kResolveAlignmentPixels));

  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();

  // Clamp to the EDRAM surface pitch (maximum possible surface pitch is also
  // assumed to be the largest resolvable size).
  int32_t surface_pitch_aligned =
      int32_t(rb_surface_info.surface_pitch &
              ~uint32_t(xenos::kResolveAlignmentPixels - 1));
  if (x1 > surface_pitch_aligned) {
    XELOGE("Resolve region {} <= x < {} is outside the surface pitch {}", x0,
           x1, surface_pitch_aligned);
    x0 = std::min(x0, surface_pitch_aligned);
    x1 = std::min(x1, surface_pitch_aligned);
  }
  assert_true(x1 - x0 <= int32_t(xenos::kMaxResolveSize));

  // Clamp the height to a sane value (to make sure it can fit in the packed
  // shader constant).
  if (y1 - y0 > int32_t(xenos::kMaxResolveSize)) {
    XELOGE("Resolve region {} <= y < {} is taller than {}", y0, y1,
           xenos::kMaxResolveSize);
    y1 = y0 + int32_t(xenos::kMaxResolveSize);
  }

  if (x0 >= x1 || y0 >= y1) {
    XELOGE("Resolve region is empty");
  }

  assert_true(x0 <= x1 && y0 <= y1);
  info_out.address.width_div_8 =
      uint32_t(x1 - x0) >> xenos::kResolveAlignmentPixelsLog2;
  info_out.address.height_div_8 =
      uint32_t(y1 - y0) >> xenos::kResolveAlignmentPixelsLog2;

  // Handle the destination.
  bool is_depth =
      rb_copy_control.copy_src_select >= xenos::kMaxColorRenderTargets;
  // Get the sample selection to safely pass to the shader.
  xenos::CopySampleSelect sample_select =
      SanitizeCopySampleSelect(rb_copy_control.copy_sample_select,
                               rb_surface_info.msaa_samples, is_depth);
  if (rb_copy_control.copy_sample_select != sample_select) {
    XELOGW(
        "Incorrect resolve sample selected for {}-sample {}: {}, treating like "
        "{}",
        1 << uint32_t(rb_surface_info.msaa_samples),
        is_depth ? "depth" : "color", rb_copy_control.copy_sample_select,
        sample_select);
  }
  info_out.address.copy_sample_select = sample_select;
  // Get the format to pass to the shader in a unified way - for depth (for
  // which Direct3D 9 specifies the k_8_8_8_8 destination format), make sure the
  // shader won't try to do conversion - pass proper k_24_8 or k_24_8_FLOAT.
  auto rb_copy_dest_info = regs.Get<reg::RB_COPY_DEST_INFO>();
  xenos::TextureFormat dest_format;
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  if (is_depth) {
    dest_format = DepthRenderTargetToTextureFormat(rb_depth_info.depth_format);
  } else {
    dest_format = xenos::TextureFormat(rb_copy_dest_info.copy_dest_format);
    // For development feedback - not much known about these formats currently.
    xenos::TextureFormat dest_closest_format;
    switch (dest_format) {
      case xenos::TextureFormat::k_8_A:
      case xenos::TextureFormat::k_8_B:
        dest_closest_format = xenos::TextureFormat::k_8;
        break;
      case xenos::TextureFormat::k_8_8_8_8_A:
        dest_closest_format = xenos::TextureFormat::k_8_8_8_8;
        break;
      default:
        dest_closest_format = dest_format;
    }
    if (dest_format != dest_closest_format) {
      XELOGW(
          "Resolving to format {}, which is untested - treating like {}. "
          "Report the game to Xenia developers!",
          FormatInfo::Get(dest_format)->name,
          FormatInfo::Get(dest_closest_format)->name);
    }
  }

  // Calculate the destination memory extent.
  uint32_t rb_copy_dest_base = regs[XE_GPU_REG_RB_COPY_DEST_BASE].u32;
  uint32_t copy_dest_base_adjusted = rb_copy_dest_base;
  uint32_t copy_dest_length;
  auto rb_copy_dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
  info_out.rb_copy_dest_pitch = rb_copy_dest_pitch;
  const FormatInfo& dest_format_info = *FormatInfo::Get(dest_format);
  if (is_depth || dest_format_info.type == FormatType::kResolvable) {
    uint32_t bpp_log2 = xe::log2_floor(dest_format_info.bits_per_pixel >> 3);
    uint32_t dest_width, dest_height, dest_depth;
    if (rb_copy_dest_info.copy_dest_array) {
      // The pointer is already adjusted to the Z / 8 (copy_dest_slice is
      // 3-bit).
      copy_dest_base_adjusted += texture_util::GetTiledOffset3D(
          x0 & ~int32_t(xenos::kTextureTileWidthHeight - 1),
          y0 & ~int32_t(xenos::kTextureTileWidthHeight - 1), 0,
          rb_copy_dest_pitch.copy_dest_pitch,
          rb_copy_dest_pitch.copy_dest_height, bpp_log2);
      // The pointer is only adjusted to Z / 8, but the texture may have a depth
      // of (N % 8) <= 4, like 4, 12, 20 when rounded up to 4
      // (xenos::kTextureTiledDepthGranularity), so provide Z + 1 to measure the
      // size of the texture conservatively, but without going out of the upper
      // bound (though this still may go out of bounds a bit probably if
      // resolving to non-zero XY, but not sure if that really happens and
      // actually causes issues).
      texture_util::GetGuestMipBlocks(
          xenos::DataDimension::k3D, rb_copy_dest_pitch.copy_dest_pitch,
          rb_copy_dest_pitch.copy_dest_height,
          rb_copy_dest_info.copy_dest_slice + 1, dest_format, 0, dest_width,
          dest_height, dest_depth);
    } else {
      copy_dest_base_adjusted += texture_util::GetTiledOffset2D(
          x0 & ~int32_t(xenos::kTextureTileWidthHeight - 1),
          y0 & ~int32_t(xenos::kTextureTileWidthHeight - 1),
          rb_copy_dest_pitch.copy_dest_pitch, bpp_log2);
      // RB_COPY_DEST_PITCH::copy_dest_height is the real texture height used
      // for 3D texture pitch, it's not relative to 0,0 of the coordinate space
      // (in Halo 3, the sniper rifle scope has copy_dest_height of 192, but the
      // rectangle's Y is 64...256) - provide the real height of the rectangle
      // since 32x32 tiles are stored linearly anyway. In addition, the height
      // in RB_COPY_DEST_PITCH may be larger than needed - in Red Dead
      // Redemption, a UI texture for the letterbox bars alpha is located within
      // the range of a 1280x720 resolve target, so with resolution scaling it's
      // also wrongly detected as scaled, while only 1280x208 is being resolved.
      texture_util::GetGuestMipBlocks(xenos::DataDimension::k2DOrStacked,
                                      rb_copy_dest_pitch.copy_dest_pitch,
                                      uint32_t(y1 - y0), 1, dest_format, 0,
                                      dest_width, dest_height, dest_depth);
    }
    copy_dest_length = texture_util::GetGuestMipSliceStorageSize(
        dest_width, dest_height, dest_depth, true, dest_format, nullptr, false);
  } else {
    XELOGE("Tried to resolve to format {}, which is not a ColorFormat",
           dest_format_info.name);
    copy_dest_length = 0;
  }
  info_out.copy_dest_base = copy_dest_base_adjusted;
  info_out.copy_dest_length = copy_dest_length;

  // Offset to 160x32 (a multiple of both the EDRAM tile size and the texture
  // tile size), so the whole offset can be stored in a very small number of
  // bits, with bases adjusted instead. The destination pointer is already
  // offset.
  uint32_t local_offset_x = uint32_t(x0) % 160;
  uint32_t local_offset_y = uint32_t(y0) & 31;
  info_out.address.local_x_div_8 =
      local_offset_x >> xenos::kResolveAlignmentPixelsLog2;
  info_out.address.local_y_div_8 =
      local_offset_y >> xenos::kResolveAlignmentPixelsLog2;
  uint32_t base_offset_x_samples =
      (uint32_t(x0) - local_offset_x)
      << uint32_t(rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X);
  uint32_t base_offset_x_tiles =
      (base_offset_x_samples + (xenos::kEdramTileWidthSamples - 1)) /
      xenos::kEdramTileWidthSamples;
  uint32_t base_offset_y_samples =
      (uint32_t(y0) - local_offset_y)
      << uint32_t(rb_surface_info.msaa_samples >= xenos::MsaaSamples::k2X);
  uint32_t base_offset_y_tiles =
      (base_offset_y_samples + (xenos::kEdramTileHeightSamples - 1)) /
      xenos::kEdramTileHeightSamples;
  uint32_t surface_pitch_tiles = xenos::GetSurfacePitchTiles(
      rb_surface_info.surface_pitch, rb_surface_info.msaa_samples, false);
  uint32_t edram_base_offset_tiles =
      base_offset_y_tiles * surface_pitch_tiles + base_offset_x_tiles;

  // Write the color/depth EDRAM info.
  bool duplicate_second_pixel =
      resolution_scale > 1 &&
      cvars::resolve_resolution_scale_duplicate_second_pixel &&
      cvars::half_pixel_offset && !regs.Get<reg::PA_SU_VTX_CNTL>().pix_center;
  int32_t exp_bias = is_depth ? 0 : rb_copy_dest_info.copy_dest_exp_bias;
  ResolveEdramPackedInfo color_edram_info;
  color_edram_info.packed = 0;
  if (!is_depth) {
    // Color.
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[rb_copy_control
                                                    .copy_src_select]);
    uint32_t is_64bpp = uint32_t(
        xenos::IsColorRenderTargetFormat64bpp(color_info.color_format));
    color_edram_info.pitch_tiles = surface_pitch_tiles << is_64bpp;
    color_edram_info.msaa_samples = rb_surface_info.msaa_samples;
    color_edram_info.is_depth = 0;
    color_edram_info.base_tiles =
        color_info.color_base + (edram_base_offset_tiles << is_64bpp);
    color_edram_info.format = uint32_t(color_info.color_format);
    color_edram_info.format_is_64bpp = is_64bpp;
    color_edram_info.duplicate_second_pixel = uint32_t(duplicate_second_pixel);
    if (edram_16_as_minus_1_to_1 &&
        (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
         color_info.color_format ==
             xenos::ColorRenderTargetFormat::k_16_16_16_16)) {
      // The texture expects 0x8001 = -32, 0x7FFF = 32, but the hack making
      // 0x8001 = -1, 0x7FFF = 1 is used - revert (this won't be correct if the
      // requested exponent bias is 27 or above, but it's a hack anyway, no need
      // to create a new copy info structure with one more bit just for this).
      exp_bias = std::min(exp_bias + int32_t(5), int32_t(31));
    }
  }
  info_out.color_edram_info = color_edram_info;
  ResolveEdramPackedInfo depth_edram_info;
  depth_edram_info.packed = 0;
  if (is_depth || rb_copy_control.depth_clear_enable) {
    depth_edram_info.pitch_tiles = surface_pitch_tiles;
    depth_edram_info.msaa_samples = rb_surface_info.msaa_samples;
    depth_edram_info.is_depth = 1;
    depth_edram_info.base_tiles =
        rb_depth_info.depth_base + edram_base_offset_tiles;
    depth_edram_info.format = uint32_t(rb_depth_info.depth_format);
    depth_edram_info.format_is_64bpp = 0;
    depth_edram_info.duplicate_second_pixel = uint32_t(duplicate_second_pixel);
  }
  info_out.depth_edram_info = depth_edram_info;

  // Patch and write RB_COPY_DEST_INFO.
  info_out.rb_copy_dest_info = rb_copy_dest_info;
  // Override with the depth format to make sure the shader doesn't have any
  // reason to try to do k_8_8_8_8 packing.
  info_out.rb_copy_dest_info.copy_dest_format = xenos::ColorFormat(dest_format);
  // Handle k_16_16 and k_16_16_16_16 range.
  info_out.rb_copy_dest_info.copy_dest_exp_bias = exp_bias;
  if (is_depth) {
    // Single component, nothing to swap.
    info_out.rb_copy_dest_info.copy_dest_swap = false;
  }

  info_out.rb_depth_clear = regs[XE_GPU_REG_RB_DEPTH_CLEAR].u32;
  info_out.rb_color_clear = regs[XE_GPU_REG_RB_COLOR_CLEAR].u32;
  info_out.rb_color_clear_lo = regs[XE_GPU_REG_RB_COLOR_CLEAR_LO].u32;

  XELOGGPU(
      "Resolve: {},{} <= x,y < {},{}, {} -> {} at 0x{:08X} (first tile at "
      "0x{:08X}, length 0x{:08X})",
      x0, y0, x1, y1,
      is_depth ? xenos::GetDepthRenderTargetFormatName(
                     xenos::DepthRenderTargetFormat(depth_edram_info.format))
               : xenos::GetColorRenderTargetFormatName(
                     xenos::ColorRenderTargetFormat(color_edram_info.format)),
      dest_format_info.name, rb_copy_dest_base, copy_dest_base_adjusted,
      copy_dest_length);

  return true;
}

ResolveCopyShaderIndex ResolveInfo::GetCopyShader(
    uint32_t resolution_scale, ResolveCopyShaderConstants& constants_out,
    uint32_t& group_count_x_out, uint32_t& group_count_y_out) const {
  ResolveCopyShaderIndex shader = ResolveCopyShaderIndex::kUnknown;
  bool is_depth = IsCopyingDepth();
  ResolveEdramPackedInfo edram_info =
      is_depth ? depth_edram_info : color_edram_info;
  if (is_depth ||
      (!rb_copy_dest_info.copy_dest_exp_bias &&
       xenos::IsSingleCopySampleSelected(address.copy_sample_select) &&
       xenos::IsColorResolveFormatBitwiseEquivalent(
           xenos::ColorRenderTargetFormat(color_edram_info.format),
           xenos::ColorFormat(rb_copy_dest_info.copy_dest_format)))) {
    bool is_64bpp = is_depth ? false : (color_edram_info.format_is_64bpp != 0);
    if (resolution_scale >= 2) {
      shader = is_64bpp ? ResolveCopyShaderIndex::kFast64bpp2xRes
                        : ResolveCopyShaderIndex::kFast32bpp2xRes;
    } else {
      if (edram_info.msaa_samples >= xenos::MsaaSamples::k4X) {
        shader = is_64bpp ? ResolveCopyShaderIndex::kFast64bpp4xMSAA
                          : ResolveCopyShaderIndex::kFast32bpp4xMSAA;
      } else {
        shader = is_64bpp ? ResolveCopyShaderIndex::kFast64bpp1x2xMSAA
                          : ResolveCopyShaderIndex::kFast32bpp1x2xMSAA;
      }
    }
  } else {
    const FormatInfo& dest_format_info = *FormatInfo::Get(
        xenos::TextureFormat(rb_copy_dest_info.copy_dest_format));
    if (resolution_scale >= 2) {
      switch (dest_format_info.bits_per_pixel) {
        case 8:
          shader = ResolveCopyShaderIndex::kFull8bpp2xRes;
          break;
        case 16:
          shader = ResolveCopyShaderIndex::kFull16bpp2xRes;
          break;
        case 32:
          shader = ResolveCopyShaderIndex::kFull32bpp2xRes;
          break;
        case 64:
          shader = ResolveCopyShaderIndex::kFull64bpp2xRes;
          break;
        case 128:
          shader = ResolveCopyShaderIndex::kFull128bpp2xRes;
          break;
      }
    } else {
      switch (dest_format_info.bits_per_pixel) {
        case 8:
          shader = ResolveCopyShaderIndex::kFull8bpp;
          break;
        case 16:
          shader = ResolveCopyShaderIndex::kFull16bpp;
          break;
        case 32:
          shader = ResolveCopyShaderIndex::kFull32bpp;
          break;
        case 64:
          shader = ResolveCopyShaderIndex::kFull64bpp;
          break;
        case 128:
          shader = ResolveCopyShaderIndex::kFull128bpp;
          break;
      }
    }
  }

  constants_out.dest_relative.edram_info = edram_info;
  constants_out.dest_relative.address_info = address;
  constants_out.dest_relative.dest_info = rb_copy_dest_info;
  constants_out.dest_relative.dest_pitch = rb_copy_dest_pitch;
  constants_out.dest_base = copy_dest_base;

  if (shader != ResolveCopyShaderIndex::kUnknown) {
    uint32_t width = address.width_div_8 << xenos::kResolveAlignmentPixelsLog2;
    uint32_t height = address.height_div_8
                      << xenos::kResolveAlignmentPixelsLog2;
    const ResolveCopyShaderInfo& shader_info =
        resolve_copy_shader_info[size_t(shader)];
    group_count_x_out = (width + ((1 << shader_info.group_size_x_log2) - 1)) >>
                        shader_info.group_size_x_log2;
    group_count_y_out = (height + ((1 << shader_info.group_size_y_log2) - 1)) >>
                        shader_info.group_size_y_log2;
  } else {
    XELOGE("No resolve copy compute shader for the provided configuration");
    assert_always();
    group_count_x_out = 0;
    group_count_y_out = 0;
  }

  return shader;
}

}  // namespace draw_util
}  // namespace gpu
}  // namespace xe

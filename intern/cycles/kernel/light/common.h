/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/types.h"

#include "kernel/sample/mapping.h"

CCL_NAMESPACE_BEGIN

/* Result from light sampling with next event estimation.
 *
 * TODO: It may be possible to reduce the size of this struct now that shader evaluation
 * no longer uses this. For example D, Ng or P, though it's not trivial. */
struct LightSample {
  float3 P;            /* position on light, or direction for distant light */
  packed_float3 Ng;    /* normal on light */
  float t;             /* distance to light (FLT_MAX for distant light) */
  float3 D;            /* direction from shading point to light */
  float pdf;           /* pdf for selecting light and point on light */
  float pdf_selection; /* pdf for selecting light */
  float eval_fac;      /* intensity multiplier (normalization, spot falloff) */
  int object;          /* object id for triangle/curve lights */
  int prim;            /* lamp id for lights, primitive id for triangle/curve lights */
  int shader;          /* shader id */
  int group;           /* lightgroup */
  LightType type;      /* type of light */
  int emitter_id;      /* index in the emitter array */
};

/* Result of evaluating a light from an intersection. */
struct LightEval {
  float eval_fac = 0.0f; /* Intensity multiplier (normalization, spot falloff) */
  float pdf = 0.0f;      /* PDF for light sampling with next event estimation sampling. */
};

/* Utilities */

/* Factor (0 at `from`, 1 at `to`) of a Map Range interpolation type, as the Map Range node. */
ccl_device_inline float light_map_range_factor(
    const int type, const float from, const float to, const float steps, const float t)
{
  switch (type) {
    case LIGHT_MAP_RANGE_STEPPED: {
      const float f = (t - from) / (to - from);
      return (steps > 0.0f) ? floorf(f * (steps + 1.0f)) / steps : 0.0f;
    }
    case LIGHT_MAP_RANGE_SMOOTHSTEP:
      return (from > to) ? 1.0f - smoothstep(to, from, t) : smoothstep(from, to, t);
    case LIGHT_MAP_RANGE_SMOOTHERSTEP: {
      const bool flip = from > to;
      const float lo = flip ? to : from;
      const float hi = flip ? from : to;
      const float x = clamp((t - lo) / (hi - lo), 0.0f, 1.0f);
      const float f = x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
      return flip ? 1.0f - f : f;
    }
    default:
      return (t - from) / (to - from);
  }
}

/* Attenuation of point, spot and area lights by `t`, the distance from the shaded point to the
 * point on the light. 1 when neither part is used.
 *
 * Map Range: up to Min Distance there is no falloff, the light is as bright as at Min Distance
 * (as Decay Start in 3ds Max), times To Min. The light falls off as usual (including the Light
 * Falloff) from there, and goes from To Min to To Max between Min and Max Distance with the
 * interpolation type of the Map Range node; from Max Distance it is times To Max.
 *
 * Light Falloff: as the Light Falloff shader node with Strength 1. */
ccl_device_inline float light_attenuation_factor(const ccl_global KernelLight *klight,
                                                 const float t)
{
  const ccl_global KernelLightAttenuation *att = &klight->attenuation;
  if (t == FLT_MAX) {
    return 1.0f;
  }
  float factor = 1.0f;
  float te = t;

  if (att->map_range_type != LIGHT_MAP_RANGE_NONE) {
    const float min_distance = att->min_distance;
    const float max_distance = att->max_distance;
    if (t >= max_distance) {
      if (att->to_max == 0.0f) {
        return 0.0f;
      }
      factor = att->to_max;
    }
    else if (t < min_distance) {
      /* The usual falloff is 1 / t^2: undo it relative to Min Distance, and evaluate the rest at
       * Min Distance, so that the light is constant inside it. */
      factor = att->to_min * sqr(t / min_distance);
      te = min_distance;
    }
    else {
      const float f = clamp(
          light_map_range_factor(att->map_range_type, min_distance, max_distance, att->steps, t),
          0.0f,
          1.0f);
      factor = att->to_min + f * (att->to_max - att->to_min);
    }
  }

  if (att->falloff_type != LIGHT_FALLOFF_NONE) {
    float strength = 1.0f;
    if (att->falloff_type == LIGHT_FALLOFF_LINEAR) {
      strength *= te;
    }
    else if (att->falloff_type == LIGHT_FALLOFF_CONSTANT) {
      strength *= te * te;
    }
    if (att->falloff_smooth > 0.0f) {
      const float squared = te * te;
      strength *= squared / (att->falloff_smooth + squared);
    }
    factor *= strength;
  }

  return factor;
}

ccl_device_inline float3 ellipse_sample(const float3 ru, const float3 rv, const float2 rand)
{
  const float2 uv = sample_uniform_disk(rand);
  return ru * uv.x + rv * uv.y;
}

ccl_device_inline float3 rectangle_sample(const float3 ru, const float3 rv, const float2 rand)
{
  return ru * (2.0f * rand.x - 1.0f) + rv * (2.0f * rand.y - 1.0f);
}

ccl_device float3 disk_light_sample(const float3 n, const float2 rand)
{
  float3 ru;
  float3 rv;

  make_orthonormals(n, &ru, &rv);

  return ellipse_sample(ru, rv, rand);
}

ccl_device float light_pdf_area_to_solid_angle(const float3 Ng, const float3 I, const float t)
{
  const float cos_pi = dot(Ng, I);

  if (cos_pi <= 0.0f) {
    return 0.0f;
  }

  return t * t / cos_pi;
}

/* Visibility flag om the light shader. */
ccl_device_inline bool is_light_shader_visible_to_path(const int shader,
                                                       const PathRayVisibility path_visibility,
                                                       const uint32_t path_flag)
{
  if ((shader & SHADER_EXCLUDE_ANY) == 0) {
    return true;
  }

  if (((shader & SHADER_EXCLUDE_DIFFUSE) && (path_visibility & PATH_RAY_VISIBILITY_DIFFUSE)) ||
      ((shader & SHADER_EXCLUDE_GLOSSY) &&
       ((path_visibility & PATH_RAY_VISIBILITY_GLOSSY) && (path_flag & PATH_RAY_REFLECT))) ||
      ((shader & SHADER_EXCLUDE_TRANSMIT) && (path_visibility & PATH_RAY_VISIBILITY_TRANSMIT)) ||
      ((shader & SHADER_EXCLUDE_CAMERA) && (path_visibility & PATH_RAY_VISIBILITY_CAMERA)) ||
      ((shader & SHADER_EXCLUDE_SCATTER) &&
       (path_visibility & PATH_RAY_VISIBILITY_VOLUME_SCATTER)))
  {
    return false;
  }

  return true;
}

CCL_NAMESPACE_END

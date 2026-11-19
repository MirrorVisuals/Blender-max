/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Decals
 *
 * A decal projects a material onto the surfaces inside its box, without changing them: a label
 * on a bottle, a sign on a wall, a stain, a crack. The empty of the decal is the box (-1 to 1 on
 * every axis): it projects along its -Z axis (planar), or wraps around its Z axis (cylindrical).
 *
 * Where a surface is inside the box, the material of the decal is evaluated after the material of
 * the surface, with the coordinates of the decal as the default UV, generated coordinates and
 * tangent (so Image Textures and Normal Maps follow the decal). Its closures are layered on top:
 * the transparency of the decal material (its alpha) shows the surface under it, the decal's
 * opacity and the fading at the edges of the projection too. The decal can keep the bump of the
 * surface under it, and have its own bump or normal map (a recessed or embossed decal). */

#pragma once

#include "kernel/globals.h"
#include "kernel/types.h"

#include "kernel/geom/object.h"

#include "kernel/svm/types.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline uint decal_object_mask(KernelGlobals kg, const int object)
{
  if (object == OBJECT_NONE || kernel_data.integrator.num_decals == 0) {
    return 0;
  }
  return kernel_data_fetch(objects, object).decal_mask;
}

/* World space differentials of the shading position. */
ccl_device_inline void decal_dP(const ccl_private ShaderData *sd,
                                ccl_private float3 *dPdx,
                                ccl_private float3 *dPdy)
{
#if defined(__RAY_DIFFERENTIALS__) && defined(__DPDU__)
  *dPdx = sd->dPdu * sd->du.dx + sd->dPdv * sd->dv.dx;
  *dPdy = sd->dPdu * sd->du.dy + sd->dPdv * sd->dv.dy;
#else
  (void)sd;
  *dPdx = zero_float3();
  *dPdy = zero_float3();
#endif
}

/* How much of the decal covers the shading point (0 where it does not), and its coordinates
 * there, written to the ShaderData for the evaluation of the decal material. */
ccl_device float decal_setup(KernelGlobals kg,
                             ccl_private ShaderData *sd,
                             const ccl_private KernelDecal &decal)
{
  const float3 p = transform_point(&decal.itfm, sd->P);
  if (fabsf(p.z) > 1.0f) {
    return 0.0f;
  }

  /* The outward normal of the surface, whichever side is seen. */
  const float3 N = (sd->flag & SD_BACKFACING) ? -sd->N : sd->N;

  float3 dPdx, dPdy;
  decal_dP(sd, &dPdx, &dPdy);
  const float3 dx = transform_direction(&decal.itfm, dPdx);
  const float3 dy = transform_direction(&decal.itfm, dPdy);

  float3 axis;
  float3 tangent;
  if (decal.flags & DECAL_CYLINDER) {
    const float r2 = p.x * p.x + p.y * p.y;
    if (r2 > 1.0f || r2 < 1e-12f) {
      return 0.0f;
    }
    /* The angle around Z, 0 towards -Y (the front of the empty). */
    const float angle = atan2f(p.x, -p.y);
    const float span = max(decal.span, 1e-4f);
    if (fabsf(angle) > 0.5f * span) {
      return 0.0f;
    }
    const float r = sqrtf(r2);
    sd->decal_uvw = make_float3(0.5f + angle / span, 0.5f * (p.z + 1.0f), r);
    sd->decal_duvw_dx = make_float3(
        (p.x * dx.y - p.y * dx.x) / (r2 * span), 0.5f * dx.z, (p.x * dx.x + p.y * dx.y) / r);
    sd->decal_duvw_dy = make_float3(
        (p.x * dy.y - p.y * dy.x) / (r2 * span), 0.5f * dy.z, (p.x * dy.x + p.y * dy.y) / r);
    axis = safe_normalize(transform_direction(&decal.tfm, make_float3(p.x, p.y, 0.0f)));
    tangent = transform_direction(&decal.tfm, make_float3(-p.y, p.x, 0.0f));
  }
  else {
    if (fabsf(p.x) > 1.0f || fabsf(p.y) > 1.0f) {
      return 0.0f;
    }
    sd->decal_uvw = 0.5f * (p + one_float3());
    sd->decal_duvw_dx = 0.5f * dx;
    sd->decal_duvw_dy = 0.5f * dy;
    /* Projected along -Z: on the surfaces facing +Z. */
    axis = safe_normalize(transform_direction(&decal.tfm, make_float3(0.0f, 0.0f, 1.0f)));
    tangent = transform_direction(&decal.tfm, make_float3(1.0f, 0.0f, 0.0f));
  }

  /* Fade out where the surface turns away from the projection. */
  float cos_angle = dot(N, axis);
  if (decal.flags & DECAL_BACK_FACES) {
    cos_angle = fabsf(cos_angle);
  }
  if (cos_angle <= decal.cos_angle_limit) {
    return 0.0f;
  }
  float coverage = decal.opacity;
  if (cos_angle < decal.cos_angle_fade) {
    coverage *= smoothstep(decal.cos_angle_limit, decal.cos_angle_fade, cos_angle);
  }
  /* Fade out at the ends of the depth. */
  if (decal.depth_fade > 0.0f) {
    coverage *= 1.0f - smoothstep(1.0f - decal.depth_fade, 1.0f, fabsf(p.z));
  }
  if (!(coverage > 1e-4f)) {
    return 0.0f;
  }

  /* The tangent along U, in the plane of the surface, in object space as the Normal Map node
   * reads it. */
  tangent = safe_normalize(tangent - N * dot(N, tangent));
  if (sd->object != OBJECT_NONE && !(sd->object_flag & SD_OBJECT_TRANSFORM_APPLIED)) {
    object_inverse_dir_transform(kg, sd, &tangent);
  }
  sd->decal_tangent = tangent;
  return coverage;
}

ccl_device_inline void decal_attribute_convert(const float3 f, ccl_private float &r)
{
  r = f.x;
}
ccl_device_inline void decal_attribute_convert(const float3 f, ccl_private float2 &r)
{
  r = make_float2(f.x, f.y);
}
ccl_device_inline void decal_attribute_convert(const float3 f, ccl_private float3 &r)
{
  r = f;
}
ccl_device_inline void decal_attribute_convert(const float3 f, ccl_private float4 &r)
{
  r = make_float4(f.x, f.y, f.z, 0.0f);
}

template<typename T>
ccl_device_inline T decal_attribute(const ccl_private ShaderData *sd,
                                    const AttributeDescriptor desc)
{
  using BaseT = dual_base_t<T>;

  float3 value = zero_float3();
  float3 dx = zero_float3();
  float3 dy = zero_float3();
  switch (desc.offset) {
    case DECAL_ATTR_UV:
      value = make_float3(sd->decal_uvw.x, sd->decal_uvw.y, 0.0f);
      dx = make_float3(sd->decal_duvw_dx.x, sd->decal_duvw_dx.y, 0.0f);
      dy = make_float3(sd->decal_duvw_dy.x, sd->decal_duvw_dy.y, 0.0f);
      break;
    case DECAL_ATTR_GENERATED:
      value = sd->decal_uvw;
      dx = sd->decal_duvw_dx;
      dy = sd->decal_duvw_dy;
      break;
    case DECAL_ATTR_TANGENT:
      value = sd->decal_tangent;
      break;
    case DECAL_ATTR_TANGENT_SIGN:
      value = make_float3(1.0f, 1.0f, 1.0f);
      break;
    default:
      break;
  }

  if constexpr (is_dual_v<T>) {
    T result;
    decal_attribute_convert(value, result.val);
#ifdef __RAY_DIFFERENTIALS__
    decal_attribute_convert(dx, result.dx);
    decal_attribute_convert(dy, result.dy);
#endif
    return result;
  }
  else {
    (void)dx;
    (void)dy;
    BaseT result;
    decal_attribute_convert(value, result);
    return result;
  }
}

/* Layering the closures of decals over the ones of the surface. */
struct DecalLayerState {
  /* The shader of the surface. */
  int shader;
  /* First closure of the decal being evaluated. */
  int first_closure;
  /* Coverage of the decal being evaluated. */
  float coverage;
  /* Emission and transparency of what is under the decal. */
  Spectrum emission;
  Spectrum transparent_extinction;
  bool transparent;
  /* The bumped normal of the surface. */
  float3 surface_N;
  bool has_surface_N;
};

/* The normal of the surface with its bump: the closures of its material. */
ccl_device_inline float3 decal_surface_normal(const ccl_private ShaderData *sd, const int end)
{
  float3 N = zero_float3();
  for (int i = 0; i < end; i++) {
    const ccl_private ShaderClosure *sc = &sd->closure[i];
    if (CLOSURE_IS_BSDF_OR_BSSRDF(sc->type) && sc->type != CLOSURE_BSDF_TRANSPARENT_ID) {
      N += sc->sample_weight * sc->N;
    }
  }
  return (len_squared(N) > 1e-12f) ? normalize(N) : sd->N;
}

/* Find the next decal on the shading point, from `start`, and prepare the evaluation of its
 * material. Returns the decal, or -1. */
ccl_device int decal_layer_begin(KernelGlobals kg,
                                 ccl_private ShaderData *sd,
                                 const int start,
                                 ccl_private DecalLayerState *state)
{
  const uint mask = decal_object_mask(kg, sd->object);
  if (mask == 0 || !(sd->type & PRIMITIVE_TRIANGLE)) {
    return -1;
  }
  const int num_decals = kernel_data.integrator.num_decals;
  for (int i = start; i < num_decals; i++) {
    if (!(mask & (1u << i))) {
      continue;
    }
    const KernelDecal decal = kernel_data_fetch(decals, i);
    if (decal.shader < 0) {
      continue;
    }
    const float coverage = decal_setup(kg, sd, decal);
    if (coverage == 0.0f) {
      continue;
    }

    if (!state->has_surface_N) {
      state->surface_N = decal_surface_normal(sd, sd->num_closure);
      state->has_surface_N = true;
    }
    state->first_closure = sd->num_closure;
    state->coverage = coverage;
    state->emission = (sd->flag & SD_EMISSION) ? sd->closure_emission_background : zero_spectrum();
    state->transparent_extinction = sd->closure_transparent_extinction;
    state->transparent = (sd->flag & SD_TRANSPARENT) != 0;

    /* The decal material adds its own emission and transparency, and its own transparent
     * closure. */
    sd->flag &= ~(SD_EMISSION | SD_TRANSPARENT);
    sd->closure_emission_background = zero_spectrum();
    sd->closure_transparent_extinction = zero_spectrum();
    sd->shader = (sd->shader & ~SHADER_MASK) | decal.shader;
    sd->object_flag |= SD_OBJECT_DECAL_EVAL;
    return i;
  }
  return -1;
}

/* Layer the closures of the decal material over what is under it. */
ccl_device void decal_layer_end(KernelGlobals kg,
                                ccl_private ShaderData *sd,
                                const int decal_index,
                                const ccl_private DecalLayerState *state)
{
  const KernelDecal decal = kernel_data_fetch(decals, decal_index);
  const float coverage = state->coverage;

  /* What shows through the decal: its transparency, its opacity and fading. */
  const Spectrum decal_transparency = saturate(sd->closure_transparent_extinction);
  const Spectrum under = one_spectrum() - coverage * (one_spectrum() - decal_transparency);
  const float under_average = average(under);

  const float3 bump_offset = decal.surface_bump * (state->surface_N - sd->N);
  for (int i = state->first_closure; i < sd->num_closure; i++) {
    ccl_private ShaderClosure *sc = &sd->closure[i];
    if (sc->type == CLOSURE_BSDF_TRANSPARENT_ID) {
      /* The transparency of the decal is the surface under it. */
      sc->weight = zero_spectrum();
      sc->sample_weight = 0.0f;
      continue;
    }
    sc->weight *= coverage;
    sc->sample_weight *= coverage;
    if (CLOSURE_IS_BSDF_OR_BSSRDF(sc->type) && decal.surface_bump > 0.0f) {
      /* The bump of the surface under the decal. */
      sc->N = safe_normalize(sc->N + bump_offset);
    }
  }
  for (int i = 0; i < state->first_closure; i++) {
    ccl_private ShaderClosure *sc = &sd->closure[i];
    sc->weight *= under;
    sc->sample_weight *= under_average;
  }

  const Spectrum decal_emission = (sd->flag & SD_EMISSION) ? sd->closure_emission_background :
                                                             zero_spectrum();
  const Spectrum emission = state->emission * under + decal_emission * coverage;
  sd->closure_emission_background = emission;
  if (!is_zero(emission)) {
    sd->flag |= SD_EMISSION;
  }
  else {
    sd->flag &= ~SD_EMISSION;
  }

  sd->closure_transparent_extinction = state->transparent_extinction * under;
  if (state->transparent) {
    sd->flag |= SD_TRANSPARENT;
  }
  else {
    sd->flag &= ~SD_TRANSPARENT;
  }

  sd->shader = (sd->shader & ~SHADER_MASK) | state->shader;
  sd->object_flag &= ~SD_OBJECT_DECAL_EVAL;
}

CCL_NAMESPACE_END

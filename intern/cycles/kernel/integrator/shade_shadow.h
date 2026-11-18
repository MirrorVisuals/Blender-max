/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/integrator/guiding.h"
#include "kernel/integrator/shade_volume.h"
#include "kernel/integrator/surface_shader.h"
#include "kernel/integrator/volume_stack.h"

#include "kernel/geom/shader_data.h"
#include "kernel/light/light.h"

#include "kernel/closure/bsdf_util.h"

#include "kernel/geom/section.h"

CCL_NAMESPACE_BEGIN

enum TransparentShadowEvalResult {
  TRANSPARENT_SHADOW_EVAL_CONTINUE = 0,
  TRANSPARENT_SHADOW_EVAL_OPAQUE = 1,
  TRANSPARENT_SHADOW_EVAL_CACHE_MISS = 2,
};

#ifdef __KERNEL_GPU__
/* Assume we can pack num hits into 12 bits, so that we can also store resume hits
 * and skip volume in the 16 bit num_hits. */
#  define SHADOW_HIT_COUNT_BITS_GPU 12
#  define SHADOW_HIT_COUNT_MASK_GPU ((1 << SHADOW_HIT_COUNT_BITS_GPU) - 1)
#  define SHADOW_RESUME_BITS_GPU 3
#  define SHADOW_RESUME_MASK_GPU ((1 << SHADOW_RESUME_BITS_GPU) - 1)

/* +1 is for the extra loop iteration for the final volume segment. */
static_assert(INTEGRATOR_SHADOW_ISECT_SIZE_GPU + 1 < (1 << SHADOW_RESUME_BITS_GPU),
              "INTEGRATOR_SHADOW_ISECT_SIZE_GPU too large for resume_hit bits");
#endif

ccl_device_inline uint shadow_num_hits_get(const uint packed_num_hits)
{
#ifdef __KERNEL_GPU__
  return packed_num_hits & SHADOW_HIT_COUNT_MASK_GPU;
#else
  return packed_num_hits;
#endif
}

ccl_device_inline uint shadow_resume_hit_get(const uint packed_num_hits)
{
#ifdef __KERNEL_GPU__
  return (packed_num_hits >> SHADOW_HIT_COUNT_BITS_GPU) & SHADOW_RESUME_MASK_GPU;
#else
  (void)packed_num_hits;
  return 0;
#endif
}

ccl_device_inline bool shadow_skip_volume_get(const uint packed_num_hits)
{
#ifdef __KERNEL_GPU__
  return (packed_num_hits >> (SHADOW_HIT_COUNT_BITS_GPU + SHADOW_RESUME_BITS_GPU)) & 1;
#else
  (void)packed_num_hits;
  return false;
#endif
}

ccl_device_inline uint shadow_num_hits_pack(const uint num_hits,
                                            const uint resume_hit,
                                            const bool skip_volume)
{
#ifdef __KERNEL_GPU__
  return (num_hits & SHADOW_HIT_COUNT_MASK_GPU) |
         ((resume_hit & SHADOW_RESUME_MASK_GPU) << SHADOW_HIT_COUNT_BITS_GPU) |
         ((skip_volume ? 1u : 0u) << (SHADOW_HIT_COUNT_BITS_GPU + SHADOW_RESUME_BITS_GPU));
#else
  /* Cache miss resume not supported on CPU. */
  kernel_assert(resume_hit == 0 && !skip_volume);
  (void)resume_hit;
  (void)skip_volume;
  return num_hits;
#endif
}

ccl_device_inline bool shadow_intersections_has_remaining(const uint packed_num_hits)
{
  return shadow_num_hits_get(packed_num_hits) >= INTEGRATOR_SHADOW_ISECT_SIZE;
}

#ifdef __TRANSPARENT_SHADOWS__

/* Quick Caustics
 *
 * Sunlight (or light from the sky) through a water surface onto what is below it, without
 * tracing caustic paths, as the quick sun caustics of V-Ray. Shadow rays of distant lights go
 * through the surface instead of being blocked by it, carrying the light that the surface
 * refracts towards the shaded point:
 *
 * The rays of a distant light are parallel. Each point of the surface bends them by its normal,
 * so a small patch of the surface sends its light to a patch of another size below it: where the
 * ripples focus the light the patch is smaller and brighter (the bright lines), where they spread
 * it larger and darker. The ratio of the two areas is the brightness of the caustic. It is
 * measured at the point where the shadow ray crosses the surface, from the smooth vertex normals
 * of the triangle (so displaced or modeled ripples, not bump mapping), on a plane through the
 * shaded point facing the light. */

/* Brightest focus, to keep the focal lines of a ripple (where the area goes to zero) finite. */
#  define QUICK_CAUSTICS_MAX_FOCUS 24.0f

ccl_device_inline float3 quick_caustics_normal(KernelGlobals kg,
                                               const ccl_private ShaderData *sd,
                                               const float u,
                                               const float v,
                                               const float3 L)
{
  float3 N = triangle_smooth_normal(kg, sd->Ng, sd->object, sd->object_flag, sd->prim, u, v);
  if (!(sd->object_flag & SD_OBJECT_TRANSFORM_APPLIED)) {
    object_normal_transform(kg, sd, &N);
  }
  /* Facing the light: the light enters the surface from that side. */
  return (dot(N, L) < 0.0f) ? -N : N;
}

/* Where the light that reaches the surface at `x` (with normal `N`), refracted, crosses the plane
 * facing the light at `height` below `x`. */
ccl_device_inline float3 quick_caustics_project(
    const float3 x, const float3 N, const float3 L, const float eta, const float height)
{
  const float cos_i = dot(L, N);
  const float k = 1.0f - eta * eta * (1.0f - cos_i * cos_i);
  const float3 T = -eta * L + (eta * cos_i - sqrtf(max(k, 0.0f))) * N;
  const float down = max(-dot(T, L), 1e-3f);
  return x + T * (height / down);
}

ccl_device_inline float3 quick_caustics_on_plane(const float3 a, const float3 L)
{
  return a - L * dot(a, L);
}

/* Brightness of the light refracted by the surface at the receiver `height` below it: 1 for a
 * flat surface, more where it focuses the light, less where it spreads it. */
ccl_device float quick_caustics_focus(KernelGlobals kg,
                                      const ccl_private ShaderData *sd,
                                      const float3 L,
                                      const float height,
                                      const float eta)
{
  if (sd->type != PRIMITIVE_TRIANGLE || !(sd->shader & SHADER_SMOOTH_NORMAL)) {
    return 1.0f;
  }

  /* Neighbors of the hit on the triangle (the normals are linear over it). Shader setup flips the
   * derivatives of back faces, undo that to match the vertex normals. */
  const float eps = 0.01f;
  const float flip = (sd->flag & SD_BACKFACING) ? -1.0f : 1.0f;
  const float3 x0 = sd->P;
  const float3 xu = x0 + sd->dPdu * (eps * flip);
  const float3 xv = x0 + sd->dPdv * (eps * flip);

  const float3 N0 = quick_caustics_normal(kg, sd, sd->u, sd->v, L);
  const float3 Nu = quick_caustics_normal(kg, sd, sd->u + eps, sd->v, L);
  const float3 Nv = quick_caustics_normal(kg, sd, sd->u, sd->v + eps, L);

  const float hu = height + dot(xu - x0, L);
  const float hv = height + dot(xv - x0, L);

  /* Cross-section of the beam at the receiver, refracted by the ripples, and by a flat surface
   * there (the face of the triangle): their ratio is the focus of the ripples alone, so a flat
   * surface lets the light through evenly whatever the angle of the light. */
  const float3 q0 = quick_caustics_project(x0, N0, L, eta, height);
  const float3 qu = quick_caustics_project(xu, Nu, L, eta, hu);
  const float3 qv = quick_caustics_project(xv, Nv, L, eta, hv);

  const float3 Nf = (dot(sd->Ng, L) < 0.0f) ? -sd->Ng : sd->Ng;
  const float3 f0 = quick_caustics_project(x0, Nf, L, eta, height);
  const float3 fu = quick_caustics_project(xu, Nf, L, eta, hu);
  const float3 fv = quick_caustics_project(xv, Nf, L, eta, hv);

  const float area_flat = len(
      cross(quick_caustics_on_plane(fu - f0, L), quick_caustics_on_plane(fv - f0, L)));
  const float area_ripples = len(
      cross(quick_caustics_on_plane(qu - q0, L), quick_caustics_on_plane(qv - q0, L)));
  if (!(area_flat > 0.0f)) {
    return 1.0f;
  }
  if (!(area_ripples * QUICK_CAUSTICS_MAX_FOCUS > area_flat)) {
    return QUICK_CAUSTICS_MAX_FOCUS;
  }
  return area_flat / area_ripples;
}

/* Light through a Quick Caustics surface, or a negative value when it does not apply (not a
 * distant light, or Quick Caustics off): then the surface casts its shadow as usual. */
ccl_device float quick_caustics_transmission(KernelGlobals kg,
                                             const ccl_private ShaderData *sd,
                                             const ccl_private Ray *ray,
                                             const float t)
{
  const KernelShader kshader = kernel_data_fetch(shaders, sd->shader & SHADER_MASK);
  const float depth = kshader.quick_caustics_depth;
  if (!(depth > 0.0f) || ray->tmax != FLT_MAX) {
    return -1.0f;
  }

  const float3 L = ray->D;
  const float ior = max(kshader.quick_caustics_ior, 1.0f);
  const float eta = 1.0f / ior;

  /* Light lost to the reflection at the surface. */
  const float3 N = quick_caustics_normal(kg, sd, sd->u, sd->v, L);
  const float transmission = 1.0f - fresnel_dielectric_cos(dot(L, N), ior);

  /* The pattern fades out towards Depth, deeper the light arrives evenly. */
  const float fade = 1.0f - smoothstep(0.75f * depth, depth, t);
  if (!(fade > 0.0f)) {
    return transmission;
  }
  const float focus = quick_caustics_focus(kg, sd, L, t, eta);
  const float brightness = max(1.0f + kshader.quick_caustics_strength * fade * (focus - 1.0f),
                               0.0f);
  return transmission * brightness;
}

ccl_device_inline Spectrum
integrate_transparent_surface_shadow(KernelGlobals kg,
                                     IntegratorShadowState state,
                                     const int hit,
                                     ccl_private ShaderEvalResult &result)
{
  PROFILING_INIT(kg, PROFILING_SHADE_SHADOW_SURFACE);

  /* TODO: does aliasing like this break automatic SoA in CUDA?
   * Should we instead store closures separate from ShaderData?
   *
   * TODO: is it better to declare this outside the loop or keep it local
   * so the compiler can see there is no dependency between iterations? */
  ShaderDataTinyStorage shadow_sd_storage;
  ccl_private ShaderData *shadow_sd = AS_SHADER_DATA(&shadow_sd_storage);

  /* Setup shader data at surface. */
  Intersection isect ccl_optional_struct_init;
  integrator_state_read_shadow_isect(state, &isect, hit);

  Ray ray ccl_optional_struct_init;
  integrator_state_read_shadow_ray(state, &ray);

  /* Sections: light goes through what they cut away, when the cut is real for the light or the
   * light is for a cap. */
  if (kernel_data.integrator.num_sections > 0 &&
      section_shadow_hit_clipped(
          kg,
          isect.object,
          ray.P + ray.D * isect.t,
          (INTEGRATOR_STATE(state, shadow_path, flag) & PATH_RAY_SECTION_FROM_CAP) != 0))
  {
    result = SHADER_EVAL_OK;
    return one_spectrum();
  }

  shader_setup_from_ray(kg, shadow_sd, &ray, &isect);

  /* Evaluate shader. */
  if (!(shadow_sd->flag & SD_HAS_ONLY_VOLUME)) {
    surface_shader_eval<KERNEL_FEATURE_NODE_MASK_SURFACE_SHADOW>(
        kg, state, shadow_sd, nullptr, PATH_RAY_VISIBILITY_SHADOW, PATH_RAY_FLAG_NONE);
    if (shadow_sd->flag & SD_CACHE_MISS) {
      result = SHADER_EVAL_CACHE_MISS;
      return zero_spectrum();
    }
  }
  else {
    INTEGRATOR_STATE_WRITE(state, shadow_path, volume_bounds_bounce) += 1;
  }

#  ifdef __VOLUME__
  /* Exit/enter volume. */
  volume_stack_enter_exit<true>(kg, state, shadow_sd);
#  endif

  /* Disable transparent shadows for ray portals */
  if (shadow_sd->flag & SD_RAY_PORTAL) {
    result = SHADER_EVAL_EMPTY;
    return zero_spectrum();
  }

  /* Compute transparency from closures. */
  result = SHADER_EVAL_OK;

  /* Quick Caustics: the light that the surface refracts, instead of its shadow. */
  const float caustics = quick_caustics_transmission(kg, shadow_sd, &ray, isect.t);
  if (caustics >= 0.0f) {
    return make_spectrum(caustics);
  }

  return surface_shader_transparency(shadow_sd);
}

#  ifdef __VOLUME__
ccl_device_inline bool integrate_transparent_volume_shadow(KernelGlobals kg,
                                                           IntegratorShadowState state,
                                                           const int hit,
                                                           const int num_recorded_hits,
                                                           ccl_private Spectrum *ccl_restrict
                                                               throughput)
{
  PROFILING_INIT(kg, PROFILING_SHADE_SHADOW_VOLUME);

  /* TODO: deduplicate with surface, or does it not matter for memory usage? */
  ShaderDataTinyStorage shadow_sd_storage;
  ccl_private ShaderData *shadow_sd = AS_SHADER_DATA(&shadow_sd_storage);

  /* Setup shader data. */
  Ray ray ccl_optional_struct_init;
  integrator_state_read_shadow_ray(state, &ray);
  ray.self.object = OBJECT_NONE;
  ray.self.prim = PRIM_NONE;
  ray.self.light_object = OBJECT_NONE;
  ray.self.light_prim = PRIM_NONE;
  /* Modify ray position and length to match current segment. */
  ray.tmin = (hit == 0) ? ray.tmin : INTEGRATOR_STATE_ARRAY(state, shadow_isect, hit - 1, t);
  ray.tmax = (hit < num_recorded_hits) ? INTEGRATOR_STATE_ARRAY(state, shadow_isect, hit, t) :
                                         ray.tmax;

  /* `object` is only needed for light tree with light linking, it is irrelevant for shadow. */
  shader_setup_from_volume(shadow_sd, &ray, OBJECT_NONE);

  if (kernel_data.integrator.volume_ray_marching) {
    const float step_size = volume_stack_step_size<true>(kg, state);
    volume_shadow_ray_marching(kg, state, &ray, shadow_sd, throughput, step_size);
  }
  else {
    volume_shadow_null_scattering(kg, state, &ray, shadow_sd, throughput);
  }

  return shadow_sd->flag & SD_CACHE_MISS;
}
#  endif

ccl_device_inline TransparentShadowEvalResult integrate_transparent_shadow(
    KernelGlobals kg, IntegratorShadowState state, const uint packed_num_hits)
{
  /* Accumulate shadow for transparent surfaces. */
  const uint num_hits = shadow_num_hits_get(packed_num_hits);
  const uint num_recorded_hits = min(num_hits, (uint)INTEGRATOR_SHADOW_ISECT_SIZE);

  /* Resume state from previous cache miss. */
  const uint resume_hit = shadow_resume_hit_get(packed_num_hits);
  const bool resume_skip_volume = shadow_skip_volume_get(packed_num_hits);

  /* Plus one to account for world volume, which has no boundary to hit but casts shadows. */
  for (uint hit = resume_hit; hit < num_recorded_hits + 1; hit++) {
    /* Skip volume if resuming after volume completed but surface had cache miss. */
    const bool skip_volume = (hit == resume_hit) && resume_skip_volume;

    /* Volume shaders. */
    if (!skip_volume &&
        (hit < num_recorded_hits || !shadow_intersections_has_remaining(packed_num_hits)))
    {
#  ifdef __VOLUME__
      if (!integrator_state_shadow_volume_stack_is_empty(kg, state)) {
        Spectrum throughput = INTEGRATOR_STATE(state, shadow_path, throughput);
        const bool cache_miss = integrate_transparent_volume_shadow(
            kg, state, hit, num_recorded_hits, &throughput);
        if (is_zero(throughput)) {
          return TRANSPARENT_SHADOW_EVAL_OPAQUE;
        }

        if (cache_miss) {
          /* Store resume state: restart at this hit, redo volume. */
          INTEGRATOR_STATE_WRITE(state, shadow_path, packed_num_hits) = shadow_num_hits_pack(
              num_hits, hit, false);
          return TRANSPARENT_SHADOW_EVAL_CACHE_MISS;
        }

        INTEGRATOR_STATE_WRITE(state, shadow_path, throughput) = throughput;
      }
#  endif
    }

    /* Surface shaders. */
    if (hit < num_recorded_hits) {
      ShaderEvalResult result = SHADER_EVAL_EMPTY;
      const Spectrum shadow = integrate_transparent_surface_shadow(kg, state, hit, result);
      if (result == SHADER_EVAL_CACHE_MISS) {
        /* Store resume state: restart at this hit, skip volume. */
        INTEGRATOR_STATE_WRITE(state, shadow_path, packed_num_hits) = shadow_num_hits_pack(
            num_hits, hit, true);
        return TRANSPARENT_SHADOW_EVAL_CACHE_MISS;
      }

      const Spectrum throughput = INTEGRATOR_STATE(state, shadow_path, throughput) * shadow;
      if (is_zero(throughput)) {
        return TRANSPARENT_SHADOW_EVAL_OPAQUE;
      }

      INTEGRATOR_STATE_WRITE(state, shadow_path, throughput) = throughput;
      INTEGRATOR_STATE_WRITE(state, shadow_path, transparent_bounce) += 1;
      INTEGRATOR_STATE_WRITE(state, shadow_path, rng_offset) += PRNG_BOUNCE_NUM;
    }

    if (INTEGRATOR_STATE(state, shadow_path, volume_bounds_bounce) > VOLUME_BOUNDS_MAX) {
      return TRANSPARENT_SHADOW_EVAL_OPAQUE;
    }

    /* Note we do not need to check max_transparent_bounce here, the number
     * of intersections is already limited and made opaque in the
     * INTERSECT_SHADOW kernel. */
  }

  if (shadow_intersections_has_remaining(packed_num_hits)) {
    /* There are more hits that we could not recorded due to memory usage,
     * adjust ray to intersect again from the last hit. */
    const float last_hit_t = INTEGRATOR_STATE_ARRAY(state, shadow_isect, num_recorded_hits - 1, t);
    INTEGRATOR_STATE_WRITE(state, shadow_ray, tmin) = intersection_t_offset(last_hit_t);
  }

  return TRANSPARENT_SHADOW_EVAL_CONTINUE;
}
#endif /* __TRANSPARENT_SHADOWS__ */

ccl_device void integrator_shade_shadow(KernelGlobals kg,
                                        IntegratorShadowState state,
                                        ccl_global float *ccl_restrict render_buffer)
{
  PROFILING_INIT(kg, PROFILING_SHADE_SHADOW_SETUP);
  const uint packed_num_hits = INTEGRATOR_STATE(state, shadow_path, packed_num_hits);

#ifdef __TRANSPARENT_SHADOWS__
  /* Evaluate transparent shadows. */
  const TransparentShadowEvalResult result = integrate_transparent_shadow(
      kg, state, packed_num_hits);
  if (result == TRANSPARENT_SHADOW_EVAL_CACHE_MISS) {
    integrator_shadow_path_cache_miss(state, DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW);
    return;
  }
  if (result == TRANSPARENT_SHADOW_EVAL_OPAQUE) {
    integrator_shadow_path_terminate(state, DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW);
    return;
  }
#endif

  if (shadow_intersections_has_remaining(packed_num_hits)) {
    /* More intersections to find, continue shadow ray. */
    integrator_shadow_path_next(
        state, DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW, DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW);
    return;
  }

  guiding_record_direct_light(kg, state);
  film_write_direct_light(kg, state, render_buffer);
  integrator_shadow_path_terminate(state, DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW);
}

CCL_NAMESPACE_END

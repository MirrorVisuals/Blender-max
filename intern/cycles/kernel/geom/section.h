/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Sections
 *
 * Architectural sections and cut-aways without changing the geometry: a section plane (or box)
 * removes the geometry on one side of it for the rays that should see the cut, while the other
 * rays still see the whole scene. So the camera sees the building cut open, while the sun and the
 * bounced light still see the walls and the roof that were cut away, and the lighting inside
 * stays as it is in the closed building (unless Affect Lighting is turned off).
 *
 * Where a camera ray crosses the section into the inside of a closed object, the first surface
 * it hits is a back face: there the cut surface (the cap) is shaded instead, on the section,
 * facing the camera. */

#pragma once

#include "kernel/globals.h"
#include "kernel/types.h"

#include "kernel/geom/object.h"

CCL_NAMESPACE_BEGIN

/* Clipped hits skipped by one ray at most (walls, floors, furniture... behind each other). */
#define SECTION_MAX_SKIPPED_HITS 32

/* Is the point clipped (removed) by the section? */
ccl_device_inline bool section_point_clipped(const ccl_private KernelSection &section,
                                             const float3 P)
{
  const float3 local = transform_point(&section.itfm, P);
  bool clipped;
  if (section.flags & SECTION_BOX) {
    /* Box: the inside is removed (or kept, inverted). */
    clipped = fabsf(local.x) <= 1.0f && fabsf(local.y) <= 1.0f && fabsf(local.z) <= 1.0f;
  }
  else {
    /* Plane: the side its normal (local Z) points to is removed (or kept, inverted). */
    clipped = local.z > 0.0f;
  }
  return (section.flags & SECTION_INVERT) ? !clipped : clipped;
}

/* Does the section cut this ray? `visibility` is the type of the ray. */
ccl_device_inline bool section_cuts_ray(const ccl_private KernelSection &section,
                                        const uint visibility,
                                        const bool from_cap)
{
  if (from_cap || (visibility & PATH_RAY_VISIBILITY_CAMERA)) {
    /* What the camera sees, and the light of the caps (whose rays start inside the cut away
     * part, which must not shadow them). */
    return true;
  }
  if (!(section.flags & SECTION_AFFECT_LIGHTING)) {
    /* The cut is real for the light too. */
    return true;
  }
  if (visibility & (PATH_RAY_VISIBILITY_GLOSSY | PATH_RAY_VISIBILITY_TRANSMIT)) {
    /* Reflections and refractions show the cut, unless only camera rays are cut. */
    return !(section.flags & SECTION_CAMERA_ONLY);
  }
  /* Diffuse bounces: the lighting of the closed building. */
  return false;
}

ccl_device_inline uint section_object_mask(KernelGlobals kg, const int object)
{
  if (object == OBJECT_NONE || kernel_data.integrator.num_sections == 0) {
    return 0;
  }
  return kernel_data_fetch(objects, object).section_mask;
}

/* Is a hit of a camera / bounce ray on the clipped part of an object? */
ccl_device bool section_hit_clipped(
    KernelGlobals kg, const int object, const float3 P, const uint visibility, const bool from_cap)
{
  const uint mask = section_object_mask(kg, object);
  if (mask == 0) {
    return false;
  }
  const int num_sections = kernel_data.integrator.num_sections;
  for (int i = 0; i < num_sections; i++) {
    if (!(mask & (1u << i))) {
      continue;
    }
    const KernelSection section = kernel_data_fetch(sections, i);
    if (section_cuts_ray(section, visibility, from_cap) && section_point_clipped(section, P)) {
      return true;
    }
  }
  return false;
}

/* Is a hit of a shadow ray on the clipped part of an object, for the light? */
ccl_device bool section_shadow_hit_clipped(KernelGlobals kg,
                                           const int object,
                                           const float3 P,
                                           const bool from_cap)
{
  const uint mask = section_object_mask(kg, object);
  if (mask == 0) {
    return false;
  }
  const int num_sections = kernel_data.integrator.num_sections;
  for (int i = 0; i < num_sections; i++) {
    if (!(mask & (1u << i))) {
      continue;
    }
    const KernelSection section = kernel_data_fetch(sections, i);
    if ((from_cap || !(section.flags & SECTION_AFFECT_LIGHTING)) &&
        section_point_clipped(section, P))
    {
      return true;
    }
  }
  return false;
}

/* World space geometric normal of a triangle hit, as shader setup computes it. */
ccl_device_inline float3 section_triangle_normal(KernelGlobals kg,
                                                 const int object,
                                                 const int prim)
{
  const uint object_flag = kernel_data_fetch(object_flag, object);
  const int position_offset = kernel_data_fetch(objects, object).position_offset;
  const uint3 tri_vindex = kernel_data_fetch(tri_vindex, prim);
  const float3 v0 = kernel_data_fetch(tri_verts, position_offset + tri_vindex.x);
  const float3 v1 = kernel_data_fetch(tri_verts, position_offset + tri_vindex.y);
  const float3 v2 = kernel_data_fetch(tri_verts, position_offset + tri_vindex.z);
  float3 Ng = object_negative_scale_applied(object_flag) ? cross(v2 - v0, v1 - v0) :
                                                           cross(v1 - v0, v2 - v0);
  if (!(object_flag & SD_OBJECT_TRANSFORM_APPLIED)) {
    const Transform itfm = object_fetch_transform(kg, object, OBJECT_INVERSE_TRANSFORM);
    Ng = transform_direction_transposed(&itfm, Ng);
  }
  return Ng;
}

/* Where along the ray it last passed from a clipped part into a kept part, before `t_max`: the
 * place of a cap. Returns the section, or -1. */
ccl_device int section_cap_find(KernelGlobals kg,
                                const int object,
                                const float3 P,
                                const float3 D,
                                const float t_min,
                                const float t_max,
                                const uint visibility,
                                const bool from_cap,
                                ccl_private float *r_t,
                                ccl_private float3 *r_N)
{
  const uint mask = section_object_mask(kg, object);
  if (mask == 0) {
    return -1;
  }

  int best = -1;
  float best_t = t_min;
  float3 best_N = zero_float3();
  const int num_sections = kernel_data.integrator.num_sections;
  for (int i = 0; i < num_sections; i++) {
    if (!(mask & (1u << i))) {
      continue;
    }
    const KernelSection section = kernel_data_fetch(sections, i);
    if (!(section.flags & SECTION_CAP) || !section_cuts_ray(section, visibility, from_cap)) {
      continue;
    }
    const bool invert = (section.flags & SECTION_INVERT) != 0;
    const float3 o = transform_point(&section.itfm, P);
    const float3 d = transform_direction(&section.itfm, D);

    float t = -1.0f;
    float3 N_local = zero_float3();
    if (section.flags & SECTION_BOX) {
      /* Slabs of the box. Its inside is clipped: the ray leaves the clipped part where it exits
       * the box. Inverted, its outside is: where it enters the box. */
      float t_near = -FLT_MAX;
      float t_far = FLT_MAX;
      int axis_near = -1;
      int axis_far = -1;
      bool valid = true;
      for (int axis = 0; axis < 3; axis++) {
        const float oa = (axis == 0) ? o.x : (axis == 1) ? o.y : o.z;
        const float da = (axis == 0) ? d.x : (axis == 1) ? d.y : d.z;
        if (fabsf(da) < 1e-12f) {
          if (fabsf(oa) > 1.0f) {
            valid = false;
          }
          continue;
        }
        float t0 = (-1.0f - oa) / da;
        float t1 = (1.0f - oa) / da;
        if (t0 > t1) {
          const float tmp = t0;
          t0 = t1;
          t1 = tmp;
        }
        if (t0 > t_near) {
          t_near = t0;
          axis_near = axis;
        }
        if (t1 < t_far) {
          t_far = t1;
          axis_far = axis;
        }
      }
      if (!valid || t_near > t_far) {
        continue;
      }
      const int axis = invert ? axis_near : axis_far;
      t = invert ? t_near : t_far;
      if (axis < 0) {
        continue;
      }
      N_local = make_float3(
          axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f);
    }
    else {
      /* Plane: from the clipped side (+Z, or -Z inverted) to the kept side. */
      if ((invert && d.z <= 0.0f) || (!invert && d.z >= 0.0f)) {
        continue;
      }
      t = -o.z / d.z;
      N_local = make_float3(0.0f, 0.0f, 1.0f);
    }

    if (t > best_t && t < t_max) {
      best = i;
      best_t = t;
      best_N = transform_direction_transposed(&section.itfm, N_local);
    }
  }

  if (best != -1) {
    best_N = safe_normalize(best_N);
    /* Facing the ray. */
    if (dot(best_N, D) > 0.0f) {
      best_N = -best_N;
    }
    *r_t = best_t;
    *r_N = best_N;
  }
  return best;
}

/* Is the closest hit a back face seen through a section: then the cap is shaded there instead.
 * Returns the section, and the distance to the cap. */
ccl_device int section_cap_at_hit(KernelGlobals kg,
                                  const ccl_private Ray *ray,
                                  const ccl_private Intersection *isect,
                                  const float t_min,
                                  const uint visibility,
                                  const bool from_cap,
                                  ccl_private float *r_t)
{
  if (isect->type != PRIMITIVE_TRIANGLE || section_object_mask(kg, isect->object) == 0) {
    return -1;
  }
  const float3 Ng = section_triangle_normal(kg, isect->object, isect->prim);
  if (!(dot(Ng, ray->D) > 0.0f)) {
    /* Front face: the ray did not start inside the object. */
    return -1;
  }
  float3 N;
  return section_cap_find(
      kg, isect->object, ray->P, ray->D, t_min, isect->t, visibility, from_cap, r_t, &N);
}

CCL_NAMESPACE_END

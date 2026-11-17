/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "util/math_float3.h"
#include "util/param.h"
#include "util/vector.h"

CCL_NAMESPACE_BEGIN

/* Light Mix: weights for the light groups in the combined image.
 *
 * The combined pass has the light of every light group once. With a Light Mix, each light group
 * pass is added again with its weight minus one, so that the light group is in the image `weight`
 * times: a weight of one leaves it unchanged, zero removes it. This only changes how the passes
 * are read, so the weights can change during and after rendering without rendering again. */
class LightMix {
 public:
  struct Group {
    /* Name of the light group, as in `Pass::lightgroup`. */
    ustring name;
    float3 weight = one_float3();

    bool operator==(const Group &other) const
    {
      return name == other.name && weight == other.weight;
    }
  };

  /* Light groups whose weight is not one. */
  vector<Group> groups;

  bool empty() const
  {
    return groups.empty();
  }

  bool operator==(const LightMix &other) const
  {
    return groups == other.groups;
  }
  bool operator!=(const LightMix &other) const
  {
    return !(*this == other);
  }
};

CCL_NAMESPACE_END

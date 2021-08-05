/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#pragma once

#include <string.h>

#include "BLI_float3.hh"
#include "BLI_utildefines.h"

#include "MEM_guardedalloc.h"

#include "DNA_node_types.h"

#include "BKE_node.h"

#include "BLT_translation.h"

#include "NOD_geometry.h"
#include "NOD_geometry_exec.hh"

#include "node_util.h"

void geo_node_type_base(
    struct bNodeType *ntype, int type, const char *name, short nclass, short flag);
bool geo_node_poll_default(struct bNodeType *ntype,
                           struct bNodeTree *ntree,
                           const char **r_disabled_hint);

namespace blender::nodes {
void update_attribute_input_socket_availabilities(bNode &node,
                                                  const StringRef name,
                                                  const GeometryNodeAttributeInputMode mode,
                                                  const bool name_is_available = true);

Array<uint32_t> get_geometry_element_ids_as_uints(const GeometryComponent &component,
                                                  const AttributeDomain domain);

void transform_mesh(Mesh *mesh,
                    const float3 translation,
                    const float3 rotation,
                    const float3 scale);

Mesh *create_cylinder_or_cone_mesh(const float radius_top,
                                   const float radius_bottom,
                                   const float depth,
                                   const int verts_num,
                                   const GeometryNodeMeshCircleFillType fill_type);

Mesh *create_cube_mesh(const float size);

/**
 * Copies the point domain attributes from `in_component` that are in the mask to `out_component`.
 */
void copy_point_attributes_based_on_mask(const GeometryComponent &in_component,
                                         GeometryComponent &result_component,
                                         Span<bool> masks,
                                         const bool invert);

struct CurveToPointsResults {
  int result_size;
  MutableSpan<float3> positions;
  MutableSpan<float> radii;
  MutableSpan<float> tilts;

  Map<std::string, GMutableSpan> point_attributes;

  MutableSpan<float3> tangents;
  MutableSpan<float3> normals;
  MutableSpan<float3> rotations;
};
/**
 * Create references for all result point cloud attributes to simplify accessing them later on.
 */
CurveToPointsResults curve_to_points_create_result_attributes(PointCloudComponent &points,
                                                              const CurveEval &curve);

void curve_create_default_rotation_attribute(Span<float3> tangents,
                                             Span<float3> normals,
                                             MutableSpan<float3> rotations);

inline bool should_add_output_attribute(const bNode &node, StringRef output_name)
{
  LISTBASE_FOREACH (bNodeSocket *, sock, &node.outputs) {
    if (sock->name == output_name) {
      return (sock->flag & SOCK_ADD_ATTRIBUTE_TO_GEOMETRY);
    }
  }
  return false;
}

inline std::string get_local_attribute_name(const StringRef tree_name,
                                            const StringRef node_name,
                                            const StringRef socket_identifier)
{
  return "local_" + tree_name + "_" + node_name + "_" + socket_identifier;
}

inline std::string get_input_attribute_name(const StringRef tree_name,
                                            const StringRef input_identifier)
{
  return "input_" + tree_name + "_" + input_identifier;
}

}  // namespace blender::nodes

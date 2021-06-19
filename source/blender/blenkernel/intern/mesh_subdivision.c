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

/** \file
 * \ingroup bke
 *
 * Functions for accessing mesh subdivision data.
 */

#include <stdio.h>

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_mesh.h"
#include "BKE_modifier.h"
#include "BKE_scene.h"

/* Know bugs:
 * - rendering issues when objects with and without subdivision share a material
 * - no subdivision/rendering update when modifying some properties
 */

int BKE_mesh_get_subdivision_level(const Scene *scene, const Mesh *me, const bool is_final_render)
{
  return get_render_subsurf_level(&scene->r, is_final_render ? me->subdiv_render_level : me->subdiv_viewport_level, false);
}

bool BKE_mesh_uses_subdivision(const Scene *scene, const Mesh *me, const bool is_final_render)
{
  /* Make sure we have some subdivision to do. */
  if (BKE_mesh_get_subdivision_level(scene, me, is_final_render) == 0) {
    return false;
  }

  return me->use_subdivision;
}

#if 0
static void subdiv_settings_init(const Scene *scene, Mesh *mesh, SubdivSettings *settings)
{
  const int requested_levels = BKE_mesh_get_subdivision_level(scene, mesh);

  settings->is_adaptive = mesh->use_limit_surface;
  settings->is_simple = mesh->subdivision_type == ME_SIMPLE_SUBSURF;
  settings->level = settings->is_simple ?
                        1 :
                        (settings->is_adaptive ? mesh->subdivision_quality : requested_levels);
  settings->use_creases = true;
  settings->fvar_linear_interpolation = BKE_subdiv_fvar_interpolation_from_uv_smooth(
      mesh->uv_smooth);
  settings->vtx_boundary_interpolation = BKE_subdiv_vtx_boundary_interpolation_from_subsurf(
      mesh->boundary_smooth);
}
static Subdiv *subdiv_descriptor_ensure(Mesh *mesh, Subdiv *subdiv, SubdivSettings *settings)
{
  /*if (mesh->edit_mesh) {
    //subdiv = BKE_subdiv_update_from_bmesh(subdiv, settings, mesh->edit_mesh->bm);
    subdiv = BKE_subdiv_update_from_mesh(subdiv, settings, mesh->edit_mesh->mesh_eval_cage);
  }
  else*/ {
    subdiv = BKE_subdiv_update_from_mesh(subdiv, settings, mesh);
  }
  return subdiv;
}
#endif

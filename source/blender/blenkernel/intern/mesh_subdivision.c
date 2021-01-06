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

#include "DNA_mesh_types.h"
#include "DNA_scene_types.h"

#include "BKE_mesh.h"
#include "BKE_scene.h"

int BKE_mesh_get_subdivision_level(const Scene *scene, Mesh *me)
{
  // TODO(@kevindietrich) : final render
  return get_render_subsurf_level(&scene->r, me->subdiv_viewport_level, false);
}

bool BKE_mesh_uses_subdivision(const Scene *scene, Mesh *me)
{
  /* Make sure we have some subdivision to do. */
  if (BKE_mesh_get_subdivision_level(scene, me) == 0) {
    return false;
  }

  return me->use_subdivision;
}

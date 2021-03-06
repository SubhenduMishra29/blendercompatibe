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
 *
 * Copyright 2011, Blender Foundation.
 */

#include "COM_SetVectorOperation.h"
#include "COM_defines.h"

namespace blender::compositor {

SetVectorOperation::SetVectorOperation()
{
  this->addOutputSocket(DataType::Vector);
  flags.is_set_operation = true;
}

void SetVectorOperation::executePixelSampled(float output[4],
                                             float /*x*/,
                                             float /*y*/,
                                             PixelSampler /*sampler*/)
{
  output[0] = vector_.x;
  output[1] = vector_.y;
  output[2] = vector_.z;
}

void SetVectorOperation::determine_canvas(const rcti &preferred_area, rcti &r_area)
{
  r_area = preferred_area;
}

}  // namespace blender::compositor

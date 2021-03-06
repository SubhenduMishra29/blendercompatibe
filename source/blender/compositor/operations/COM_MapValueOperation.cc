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

#include "COM_MapValueOperation.h"

namespace blender::compositor {

MapValueOperation::MapValueOperation()
{
  this->addInputSocket(DataType::Value);
  this->addOutputSocket(DataType::Value);
  this->m_inputOperation = nullptr;
  flags.can_be_constant = true;
}

void MapValueOperation::initExecution()
{
  this->m_inputOperation = this->getInputSocketReader(0);
}

void MapValueOperation::executePixelSampled(float output[4],
                                            float x,
                                            float y,
                                            PixelSampler sampler)
{
  float src[4];
  this->m_inputOperation->readSampled(src, x, y, sampler);
  TexMapping *texmap = this->m_settings;
  float value = (src[0] + texmap->loc[0]) * texmap->size[0];
  if (texmap->flag & TEXMAP_CLIP_MIN) {
    if (value < texmap->min[0]) {
      value = texmap->min[0];
    }
  }
  if (texmap->flag & TEXMAP_CLIP_MAX) {
    if (value > texmap->max[0]) {
      value = texmap->max[0];
    }
  }

  output[0] = value;
}

void MapValueOperation::deinitExecution()
{
  this->m_inputOperation = nullptr;
}

void MapValueOperation::update_memory_buffer_partial(MemoryBuffer *output,
                                                     const rcti &area,
                                                     Span<MemoryBuffer *> inputs)
{
  for (BuffersIterator<float> it = output->iterate_with(inputs, area); !it.is_end(); ++it) {
    const float input = *it.in(0);
    TexMapping *texmap = this->m_settings;
    float value = (input + texmap->loc[0]) * texmap->size[0];
    if (texmap->flag & TEXMAP_CLIP_MIN) {
      if (value < texmap->min[0]) {
        value = texmap->min[0];
      }
    }
    if (texmap->flag & TEXMAP_CLIP_MAX) {
      if (value > texmap->max[0]) {
        value = texmap->max[0];
      }
    }

    it.out[0] = value;
  }
}

}  // namespace blender::compositor

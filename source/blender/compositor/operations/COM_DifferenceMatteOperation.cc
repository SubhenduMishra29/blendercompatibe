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

#include "COM_DifferenceMatteOperation.h"
#include "BLI_math.h"

namespace blender::compositor {

DifferenceMatteOperation::DifferenceMatteOperation()
{
  addInputSocket(DataType::Color);
  addInputSocket(DataType::Color);
  addOutputSocket(DataType::Value);

  this->m_inputImage1Program = nullptr;
  this->m_inputImage2Program = nullptr;
  flags.can_be_constant = true;
}

void DifferenceMatteOperation::initExecution()
{
  this->m_inputImage1Program = this->getInputSocketReader(0);
  this->m_inputImage2Program = this->getInputSocketReader(1);
}
void DifferenceMatteOperation::deinitExecution()
{
  this->m_inputImage1Program = nullptr;
  this->m_inputImage2Program = nullptr;
}

void DifferenceMatteOperation::executePixelSampled(float output[4],
                                                   float x,
                                                   float y,
                                                   PixelSampler sampler)
{
  float inColor1[4];
  float inColor2[4];

  const float tolerance = this->m_settings->t1;
  const float falloff = this->m_settings->t2;
  float difference;
  float alpha;

  this->m_inputImage1Program->readSampled(inColor1, x, y, sampler);
  this->m_inputImage2Program->readSampled(inColor2, x, y, sampler);

  difference = (fabsf(inColor2[0] - inColor1[0]) + fabsf(inColor2[1] - inColor1[1]) +
                fabsf(inColor2[2] - inColor1[2]));

  /* average together the distances */
  difference = difference / 3.0f;

  /* make 100% transparent */
  if (difference <= tolerance) {
    output[0] = 0.0f;
  }
  /* In the falloff region, make partially transparent. */
  else if (difference <= falloff + tolerance) {
    difference = difference - tolerance;
    alpha = difference / falloff;
    /* Only change if more transparent than before. */
    if (alpha < inColor1[3]) {
      output[0] = alpha;
    }
    else { /* leave as before */
      output[0] = inColor1[3];
    }
  }
  else {
    /* foreground object */
    output[0] = inColor1[3];
  }
}

void DifferenceMatteOperation::update_memory_buffer_partial(MemoryBuffer *output,
                                                            const rcti &area,
                                                            Span<MemoryBuffer *> inputs)
{
  for (BuffersIterator<float> it = output->iterate_with(inputs, area); !it.is_end(); ++it) {
    const float *color1 = it.in(0);
    const float *color2 = it.in(1);

    float difference = (fabsf(color2[0] - color1[0]) + fabsf(color2[1] - color1[1]) +
                        fabsf(color2[2] - color1[2]));

    /* Average together the distances. */
    difference = difference / 3.0f;

    const float tolerance = m_settings->t1;
    const float falloff = m_settings->t2;

    /* Make 100% transparent. */
    if (difference <= tolerance) {
      it.out[0] = 0.0f;
    }
    /* In the falloff region, make partially transparent. */
    else if (difference <= falloff + tolerance) {
      difference = difference - tolerance;
      const float alpha = difference / falloff;
      /* Only change if more transparent than before. */
      if (alpha < color1[3]) {
        it.out[0] = alpha;
      }
      else { /* Leave as before. */
        it.out[0] = color1[3];
      }
    }
    else {
      /* Foreground object. */
      it.out[0] = color1[3];
    }
  }
}

}  // namespace blender::compositor

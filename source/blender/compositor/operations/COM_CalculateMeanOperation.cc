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

#include "COM_CalculateMeanOperation.h"
#include "BLI_math.h"
#include "BLI_utildefines.h"
#include "COM_ExecutionSystem.h"

#include "IMB_colormanagement.h"

namespace blender::compositor {

CalculateMeanOperation::CalculateMeanOperation()
{
  this->addInputSocket(DataType::Color, ResizeMode::Align);
  this->addOutputSocket(DataType::Value);
  this->m_imageReader = nullptr;
  this->m_iscalculated = false;
  this->m_setting = 1;
  this->flags.complex = true;
}
void CalculateMeanOperation::initExecution()
{
  this->m_imageReader = this->getInputSocketReader(0);
  this->m_iscalculated = false;
  NodeOperation::initMutex();
}

void CalculateMeanOperation::executePixel(float output[4], int /*x*/, int /*y*/, void * /*data*/)
{
  output[0] = this->m_result;
}

void CalculateMeanOperation::deinitExecution()
{
  this->m_imageReader = nullptr;
  NodeOperation::deinitMutex();
}

bool CalculateMeanOperation::determineDependingAreaOfInterest(rcti * /*input*/,
                                                              ReadBufferOperation *readOperation,
                                                              rcti *output)
{
  rcti imageInput;
  if (this->m_iscalculated) {
    return false;
  }
  NodeOperation *operation = getInputOperation(0);
  imageInput.xmax = operation->getWidth();
  imageInput.xmin = 0;
  imageInput.ymax = operation->getHeight();
  imageInput.ymin = 0;
  if (operation->determineDependingAreaOfInterest(&imageInput, readOperation, output)) {
    return true;
  }
  return false;
}

void *CalculateMeanOperation::initializeTileData(rcti *rect)
{
  lockMutex();
  if (!this->m_iscalculated) {
    MemoryBuffer *tile = (MemoryBuffer *)this->m_imageReader->initializeTileData(rect);
    calculateMean(tile);
    this->m_iscalculated = true;
  }
  unlockMutex();
  return nullptr;
}

void CalculateMeanOperation::calculateMean(MemoryBuffer *tile)
{
  this->m_result = 0.0f;
  float *buffer = tile->getBuffer();
  int size = tile->getWidth() * tile->getHeight();
  int pixels = 0;
  float sum = 0.0f;
  for (int i = 0, offset = 0; i < size; i++, offset += 4) {
    if (buffer[offset + 3] > 0) {
      pixels++;

      switch (this->m_setting) {
        case 1: {
          sum += IMB_colormanagement_get_luminance(&buffer[offset]);
          break;
        }
        case 2: {
          sum += buffer[offset];
          break;
        }
        case 3: {
          sum += buffer[offset + 1];
          break;
        }
        case 4: {
          sum += buffer[offset + 2];
          break;
        }
        case 5: {
          float yuv[3];
          rgb_to_yuv(buffer[offset],
                     buffer[offset + 1],
                     buffer[offset + 2],
                     &yuv[0],
                     &yuv[1],
                     &yuv[2],
                     BLI_YUV_ITU_BT709);
          sum += yuv[0];
          break;
        }
      }
    }
  }
  this->m_result = sum / pixels;
}

void CalculateMeanOperation::setSetting(int setting)
{
  this->m_setting = setting;
  switch (setting) {
    case 1: {
      setting_func_ = IMB_colormanagement_get_luminance;
      break;
    }
    case 2: {
      setting_func_ = [](const float *elem) { return elem[0]; };
      break;
    }
    case 3: {
      setting_func_ = [](const float *elem) { return elem[1]; };
      break;
    }
    case 4: {
      setting_func_ = [](const float *elem) { return elem[2]; };
      break;
    }
    case 5: {
      setting_func_ = [](const float *elem) {
        float yuv[3];
        rgb_to_yuv(elem[0], elem[1], elem[2], &yuv[0], &yuv[1], &yuv[2], BLI_YUV_ITU_BT709);
        return yuv[0];
      };
      break;
    }
  }
}

void CalculateMeanOperation::get_area_of_interest(int input_idx,
                                                  const rcti &UNUSED(output_area),
                                                  rcti &r_input_area)
{
  BLI_assert(input_idx == 0);
  r_input_area = get_input_operation(input_idx)->get_canvas();
}

void CalculateMeanOperation::update_memory_buffer_started(MemoryBuffer *UNUSED(output),
                                                          const rcti &UNUSED(area),
                                                          Span<MemoryBuffer *> inputs)
{
  if (!this->m_iscalculated) {
    MemoryBuffer *input = inputs[0];
    m_result = calc_mean(input);
    this->m_iscalculated = true;
  }
}

void CalculateMeanOperation::update_memory_buffer_partial(MemoryBuffer *output,
                                                          const rcti &area,
                                                          Span<MemoryBuffer *> UNUSED(inputs))
{
  output->fill(area, &m_result);
}

float CalculateMeanOperation::calc_mean(const MemoryBuffer *input)
{
  PixelsSum total = {0};
  exec_system_->execute_work<PixelsSum>(
      input->get_rect(),
      [=](const rcti &split) { return calc_area_sum(input, split); },
      total,
      [](PixelsSum &join, const PixelsSum &chunk) {
        join.sum += chunk.sum;
        join.num_pixels += chunk.num_pixels;
      });
  return total.num_pixels == 0 ? 0.0f : total.sum / total.num_pixels;
}

using PixelsSum = CalculateMeanOperation::PixelsSum;
PixelsSum CalculateMeanOperation::calc_area_sum(const MemoryBuffer *input, const rcti &area)
{
  PixelsSum result = {0};
  for (const float *elem : input->get_buffer_area(area)) {
    if (elem[3] <= 0.0f) {
      continue;
    }
    result.sum += setting_func_(elem);
    result.num_pixels++;
  }
  return result;
}

}  // namespace blender::compositor

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

#pragma once

#include "COM_MultiThreadedOperation.h"

namespace blender::compositor {

class RotateOperation : public MultiThreadedOperation {
 private:
  constexpr static int IMAGE_INPUT_INDEX = 0;
  constexpr static int DEGREE_INPUT_INDEX = 1;

  SocketReader *m_imageSocket;
  SocketReader *m_degreeSocket;
  /* TODO(manzanilla): to be removed with tiled implementation. */
  float m_centerX;
  float m_centerY;

  float m_cosine;
  float m_sine;
  bool m_doDegree2RadConversion;
  bool m_isDegreeSet;
  PixelSampler sampler_;

 public:
  RotateOperation();

  static void rotate_coords(
      float &x, float &y, float center_x, float center_y, float sine, float cosine)
  {
    const float dx = x - center_x;
    const float dy = y - center_y;
    x = center_x + (cosine * dx + sine * dy);
    y = center_y + (-sine * dx + cosine * dy);
  }

  static void get_rotation_center(const rcti &area, float &r_x, float &r_y);
  static void get_rotation_offset(const rcti &input_canvas,
                                  const rcti &rotate_canvas,
                                  float &r_offset_x,
                                  float &r_offset_y);
  static void get_area_rotation_bounds(const rcti &area,
                                       const float center_x,
                                       const float center_y,
                                       const float sine,
                                       const float cosine,
                                       rcti &r_bounds);
  static void get_area_rotation_bounds_inverted(const rcti &area,
                                                const float center_x,
                                                const float center_y,
                                                const float sine,
                                                const float cosine,
                                                rcti &r_bounds);
  static void get_rotation_area_of_interest(const rcti &input_canvas,
                                            const rcti &rotate_canvas,
                                            const float sine,
                                            const float cosine,
                                            const rcti &output_area,
                                            rcti &r_input_area);
  static void get_rotation_canvas(const rcti &input_canvas,
                                  const float sine,
                                  const float cosine,
                                  rcti &r_canvas);

  bool determineDependingAreaOfInterest(rcti *input,
                                        ReadBufferOperation *readOperation,
                                        rcti *output) override;
  void executePixelSampled(float output[4], float x, float y, PixelSampler sampler) override;
  void init_data() override;
  void initExecution() override;
  void deinitExecution() override;

  void setDoDegree2RadConversion(bool abool)
  {
    this->m_doDegree2RadConversion = abool;
  }

  void set_sampler(PixelSampler sampler)
  {
    sampler_ = sampler;
  }

  void ensureDegree();

  void get_area_of_interest(int input_idx, const rcti &output_area, rcti &r_input_area) override;
  void update_memory_buffer_partial(MemoryBuffer *output,
                                    const rcti &area,
                                    Span<MemoryBuffer *> inputs) override;

  void determine_canvas(const rcti &preferred_area, rcti &r_area) override;
};

}  // namespace blender::compositor

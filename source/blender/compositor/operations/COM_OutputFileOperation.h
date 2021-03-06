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

#include "BLI_path_util.h"
#include "BLI_rect.h"

#include "DNA_color_types.h"

#include "intern/openexr/openexr_multi.h"

namespace blender::compositor {

/* Writes the image to a single-layer file. */
class OutputSingleLayerOperation : public MultiThreadedOperation {
 protected:
  const RenderData *m_rd;
  const bNodeTree *m_tree;

  ImageFormatData *m_format;
  char m_path[FILE_MAX];

  float *m_outputBuffer;
  DataType m_datatype;
  SocketReader *m_imageInput;

  const ColorManagedViewSettings *m_viewSettings;
  const ColorManagedDisplaySettings *m_displaySettings;

  const char *m_viewName;
  bool m_saveAsRender;

 public:
  OutputSingleLayerOperation(const RenderData *rd,
                             const bNodeTree *tree,
                             DataType datatype,
                             ImageFormatData *format,
                             const char *path,
                             const ColorManagedViewSettings *viewSettings,
                             const ColorManagedDisplaySettings *displaySettings,
                             const char *viewName,
                             const bool saveAsRender);

  void executeRegion(rcti *rect, unsigned int tileNumber) override;
  bool isOutputOperation(bool /*rendering*/) const override
  {
    return true;
  }
  void initExecution() override;
  void deinitExecution() override;
  eCompositorPriority getRenderPriority() const override
  {
    return eCompositorPriority::Low;
  }

  void update_memory_buffer_partial(MemoryBuffer *output,
                                    const rcti &area,
                                    Span<MemoryBuffer *> inputs) override;
};

/* extra info for OpenEXR layers */
struct OutputOpenExrLayer {
  OutputOpenExrLayer(const char *name, DataType datatype, bool use_layer);

  char name[EXR_TOT_MAXNAME - 2];
  DataType datatype;
  bool use_layer;

  /* internals */
  float *outputBuffer;
  SocketReader *imageInput;
};

/* Writes inputs into OpenEXR multilayer channels. */
class OutputOpenExrMultiLayerOperation : public MultiThreadedOperation {
 protected:
  const Scene *m_scene;
  const RenderData *m_rd;
  const bNodeTree *m_tree;

  char m_path[FILE_MAX];
  char m_exr_codec;
  bool m_exr_half_float;
  Vector<OutputOpenExrLayer> m_layers;
  const char *m_viewName;

  StampData *createStampData() const;

 public:
  OutputOpenExrMultiLayerOperation(const Scene *scene,
                                   const RenderData *rd,
                                   const bNodeTree *tree,
                                   const char *path,
                                   char exr_codec,
                                   bool exr_half_float,
                                   const char *viewName);

  void add_layer(const char *name, DataType datatype, bool use_layer);

  void executeRegion(rcti *rect, unsigned int tileNumber) override;
  bool isOutputOperation(bool /*rendering*/) const override
  {
    return true;
  }
  void initExecution() override;
  void deinitExecution() override;
  eCompositorPriority getRenderPriority() const override
  {
    return eCompositorPriority::Low;
  }

  void update_memory_buffer_partial(MemoryBuffer *output,
                                    const rcti &area,
                                    Span<MemoryBuffer *> inputs) override;
};

void add_exr_channels(void *exrhandle,
                      const char *layerName,
                      const DataType datatype,
                      const char *viewName,
                      const size_t width,
                      bool use_half_float,
                      float *buf);
void free_exr_channels(void *exrhandle,
                       const RenderData *rd,
                       const char *layerName,
                       const DataType datatype);
int get_datatype_size(DataType datatype);

}  // namespace blender::compositor

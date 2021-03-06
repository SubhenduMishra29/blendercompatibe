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
 * Copyright 2021, Blender Foundation.
 */

#include "COM_FullFrameExecutionModel.h"
#include "COM_Debug.h"
#include "COM_ExecutionGroup.h"
#include "COM_ReadBufferOperation.h"
#include "COM_ViewerOperation.h"
#include "COM_WorkScheduler.h"

#include "BLT_translation.h"

#ifdef WITH_CXX_GUARDEDALLOC
#  include "MEM_guardedalloc.h"
#endif

namespace blender::compositor {

FullFrameExecutionModel::FullFrameExecutionModel(CompositorContext &context,
                                                 SharedOperationBuffers &shared_buffers,
                                                 Span<NodeOperation *> operations)
    : ExecutionModel(context, operations),
      active_buffers_(shared_buffers),
      num_operations_finished_(0)
{
  priorities_.append(eCompositorPriority::High);
  if (!context.isFastCalculation()) {
    priorities_.append(eCompositorPriority::Medium);
    priorities_.append(eCompositorPriority::Low);
  }
}

void FullFrameExecutionModel::execute(ExecutionSystem &exec_system)
{
  const bNodeTree *node_tree = this->context_.getbNodeTree();
  node_tree->stats_draw(node_tree->sdh, TIP_("Compositing | Initializing execution"));

  DebugInfo::graphviz(&exec_system, "compositor_prior_rendering");

  determine_areas_to_render_and_reads();
  render_operations();
}

void FullFrameExecutionModel::determine_areas_to_render_and_reads()
{
  const bool is_rendering = context_.isRendering();
  const bNodeTree *node_tree = context_.getbNodeTree();

  rcti area;
  for (eCompositorPriority priority : priorities_) {
    for (NodeOperation *op : operations_) {
      op->setbNodeTree(node_tree);
      if (op->isOutputOperation(is_rendering) && op->getRenderPriority() == priority) {
        get_output_render_area(op, area);
        determine_areas_to_render(op, area);
        determine_reads(op);
      }
    }
  }
}

/**
 * Returns input buffers with an offset relative to given output coordinates. Returned memory
 * buffers must be deleted.
 */
Vector<MemoryBuffer *> FullFrameExecutionModel::get_input_buffers(NodeOperation *op,
                                                                  const int output_x,
                                                                  const int output_y)
{
  const int num_inputs = op->getNumberOfInputSockets();
  Vector<MemoryBuffer *> inputs_buffers(num_inputs);
  for (int i = 0; i < num_inputs; i++) {
    NodeOperation *input = op->get_input_operation(i);
    const int offset_x = (input->get_canvas().xmin - op->get_canvas().xmin) + output_x;
    const int offset_y = (input->get_canvas().ymin - op->get_canvas().ymin) + output_y;
    MemoryBuffer *buf = active_buffers_.get_rendered_buffer(input);

    rcti rect = buf->get_rect();
    BLI_rcti_translate(&rect, offset_x, offset_y);
    inputs_buffers[i] = new MemoryBuffer(
        buf->getBuffer(), buf->get_num_channels(), rect, buf->is_a_single_elem());
  }
  return inputs_buffers;
}

MemoryBuffer *FullFrameExecutionModel::create_operation_buffer(NodeOperation *op,
                                                               const int output_x,
                                                               const int output_y)
{
  rcti rect;
  BLI_rcti_init(&rect, output_x, output_x + op->getWidth(), output_y, output_y + op->getHeight());

  const DataType data_type = op->getOutputSocket(0)->getDataType();
  const bool is_a_single_elem = op->get_flags().is_constant_operation;
  return new MemoryBuffer(data_type, rect, is_a_single_elem);
}

void FullFrameExecutionModel::render_operation(NodeOperation *op)
{
  /* Output has no offset for easier image algorithms implementation on operations. */
  constexpr int output_x = 0;
  constexpr int output_y = 0;

  const bool has_outputs = op->getNumberOfOutputSockets() > 0;
  MemoryBuffer *op_buf = has_outputs ? create_operation_buffer(op, output_x, output_y) : nullptr;
  if (op->getWidth() > 0 && op->getHeight() > 0) {
    Vector<MemoryBuffer *> input_bufs = get_input_buffers(op, output_x, output_y);
    const int op_offset_x = output_x - op->get_canvas().xmin;
    const int op_offset_y = output_y - op->get_canvas().ymin;
    Span<rcti> areas = active_buffers_.get_areas_to_render(op, op_offset_x, op_offset_y);
    op->render(op_buf, areas, input_bufs);
    DebugInfo::operation_rendered(op, op_buf);

    for (MemoryBuffer *buf : input_bufs) {
      delete buf;
    }
  }
  /* Even if operation has no resolution set the empty buffer. It will be clipped with a
   * TranslateOperation from convert resolutions if linked to an operation with resolution. */
  active_buffers_.set_rendered_buffer(op, std::unique_ptr<MemoryBuffer>(op_buf));

  operation_finished(op);
}

/**
 * Render output operations in order of priority.
 */
void FullFrameExecutionModel::render_operations()
{
  const bool is_rendering = context_.isRendering();

  WorkScheduler::start(this->context_);
  for (eCompositorPriority priority : priorities_) {
    for (NodeOperation *op : operations_) {
      const bool has_size = op->getWidth() > 0 && op->getHeight() > 0;
      const bool is_priority_output = op->isOutputOperation(is_rendering) &&
                                      op->getRenderPriority() == priority;
      if (is_priority_output && has_size) {
        render_output_dependencies(op);
        render_operation(op);
      }
      else if (is_priority_output && !has_size && op->isActiveViewerOutput()) {
        static_cast<ViewerOperation *>(op)->clear_display_buffer();
      }
    }
  }
  WorkScheduler::stop();
}

/**
 * Returns all dependencies from inputs to outputs. A dependency may be repeated when
 * several operations depend on it.
 */
static Vector<NodeOperation *> get_operation_dependencies(NodeOperation *operation)
{
  /* Get dependencies from outputs to inputs. */
  Vector<NodeOperation *> dependencies;
  Vector<NodeOperation *> next_outputs;
  next_outputs.append(operation);
  while (next_outputs.size() > 0) {
    Vector<NodeOperation *> outputs(next_outputs);
    next_outputs.clear();
    for (NodeOperation *output : outputs) {
      for (int i = 0; i < output->getNumberOfInputSockets(); i++) {
        next_outputs.append(output->get_input_operation(i));
      }
    }
    dependencies.extend(next_outputs);
  }

  /* Reverse to get dependencies from inputs to outputs. */
  std::reverse(dependencies.begin(), dependencies.end());

  return dependencies;
}

void FullFrameExecutionModel::render_output_dependencies(NodeOperation *output_op)
{
  BLI_assert(output_op->isOutputOperation(context_.isRendering()));
  Vector<NodeOperation *> dependencies = get_operation_dependencies(output_op);
  for (NodeOperation *op : dependencies) {
    if (!active_buffers_.is_operation_rendered(op)) {
      render_operation(op);
    }
  }
}

/**
 * Determines all operations areas needed to render given output area.
 */
void FullFrameExecutionModel::determine_areas_to_render(NodeOperation *output_op,
                                                        const rcti &output_area)
{
  BLI_assert(output_op->isOutputOperation(context_.isRendering()));

  Vector<std::pair<NodeOperation *, const rcti>> stack;
  stack.append({output_op, output_area});
  while (stack.size() > 0) {
    std::pair<NodeOperation *, rcti> pair = stack.pop_last();
    NodeOperation *operation = pair.first;
    const rcti &render_area = pair.second;
    if (BLI_rcti_is_empty(&render_area) ||
        active_buffers_.is_area_registered(operation, render_area)) {
      continue;
    }

    active_buffers_.register_area(operation, render_area);

    const int num_inputs = operation->getNumberOfInputSockets();
    for (int i = 0; i < num_inputs; i++) {
      NodeOperation *input_op = operation->get_input_operation(i);
      rcti input_area;
      operation->get_area_of_interest(input_op, render_area, input_area);

      /* Ensure area of interest is within operation bounds, cropping areas outside. */
      BLI_rcti_isect(&input_area, &input_op->get_canvas(), &input_area);

      stack.append({input_op, input_area});
    }
  }
}

/**
 * Determines reads to receive by operations in output operation tree (i.e: Number of dependent
 * operations each operation has).
 */
void FullFrameExecutionModel::determine_reads(NodeOperation *output_op)
{
  BLI_assert(output_op->isOutputOperation(context_.isRendering()));

  Vector<NodeOperation *> stack;
  stack.append(output_op);
  while (stack.size() > 0) {
    NodeOperation *operation = stack.pop_last();
    const int num_inputs = operation->getNumberOfInputSockets();
    for (int i = 0; i < num_inputs; i++) {
      NodeOperation *input_op = operation->get_input_operation(i);
      if (!active_buffers_.has_registered_reads(input_op)) {
        stack.append(input_op);
      }
      active_buffers_.register_read(input_op);
    }
  }
}

/**
 * Calculates given output operation area to be rendered taking into account viewer and render
 * borders.
 */
void FullFrameExecutionModel::get_output_render_area(NodeOperation *output_op, rcti &r_area)
{
  BLI_assert(output_op->isOutputOperation(context_.isRendering()));

  /* By default return operation bounds (no border). */
  rcti canvas = output_op->get_canvas();
  r_area = canvas;

  const bool has_viewer_border = border_.use_viewer_border &&
                                 (output_op->get_flags().is_viewer_operation ||
                                  output_op->get_flags().is_preview_operation);
  const bool has_render_border = border_.use_render_border;
  if (has_viewer_border || has_render_border) {
    /* Get border with normalized coordinates. */
    const rctf *norm_border = has_viewer_border ? border_.viewer_border : border_.render_border;

    /* Return de-normalized border within canvas. */
    const int w = output_op->getWidth();
    const int h = output_op->getHeight();
    r_area.xmin = canvas.xmin + norm_border->xmin * w;
    r_area.xmax = canvas.xmin + norm_border->xmax * w;
    r_area.ymin = canvas.ymin + norm_border->ymin * h;
    r_area.ymax = canvas.ymin + norm_border->ymax * h;
  }
}

void FullFrameExecutionModel::operation_finished(NodeOperation *operation)
{
  /* Report inputs reads so that buffers may be freed/reused. */
  const int num_inputs = operation->getNumberOfInputSockets();
  for (int i = 0; i < num_inputs; i++) {
    active_buffers_.read_finished(operation->get_input_operation(i));
  }

  num_operations_finished_++;
  update_progress_bar();
}

void FullFrameExecutionModel::update_progress_bar()
{
  const bNodeTree *tree = context_.getbNodeTree();
  if (tree) {
    const float progress = num_operations_finished_ / static_cast<float>(operations_.size());
    tree->progress(tree->prh, progress);

    char buf[128];
    BLI_snprintf(buf,
                 sizeof(buf),
                 TIP_("Compositing | Operation %i-%li"),
                 num_operations_finished_ + 1,
                 operations_.size());
    tree->stats_draw(tree->sdh, buf);
  }
}

}  // namespace blender::compositor

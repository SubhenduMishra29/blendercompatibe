/*
 * Copyright 2011-2021 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "integrator/path_trace_work_gpu.h"

#include "device/device.h"

#include "integrator/pass_accessor_gpu.h"
#include "render/buffers.h"
#include "render/gpu_display.h"
#include "render/scene.h"
#include "util/util_logging.h"
#include "util/util_tbb.h"
#include "util/util_time.h"

#include "kernel/kernel_types.h"

CCL_NAMESPACE_BEGIN

PathTraceWorkGPU::PathTraceWorkGPU(Device *device,
                                   DeviceScene *device_scene,
                                   RenderBuffers *buffers,
                                   bool *cancel_requested_flag)
    : PathTraceWork(device, device_scene, buffers, cancel_requested_flag),
      queue_(device->gpu_queue_create()),
      render_buffers_(buffers),
      integrator_queue_counter_(device, "integrator_queue_counter", MEM_READ_WRITE),
      integrator_shader_sort_counter_(device, "integrator_shader_sort_counter", MEM_READ_WRITE),
      integrator_shader_raytrace_sort_counter_(
          device, "integrator_shader_raytrace_sort_counter", MEM_READ_WRITE),
      queued_paths_(device, "queued_paths", MEM_READ_WRITE),
      num_queued_paths_(device, "num_queued_paths", MEM_READ_WRITE),
      work_tiles_(device, "work_tiles", MEM_READ_WRITE),
      gpu_display_rgba_half_(device, "display buffer half", MEM_READ_WRITE),
      max_num_paths_(queue_->num_concurrent_states(sizeof(IntegratorState))),
      min_num_active_paths_(queue_->num_concurrent_busy_states()),
      max_active_path_index_(0)
{
  memset(&integrator_state_gpu_, 0, sizeof(integrator_state_gpu_));
}

void PathTraceWorkGPU::alloc_integrator_soa()
{
  /* IntegrateState allocated as structure of arrays.
   *
   * Allocate a device only memory buffer before for each struct member, and then
   * write the pointers into a struct that resides in constant memory.
   *
   * TODO: store float3 in separate XYZ arrays. */

  if (!integrator_state_soa_.empty()) {
    return;
  }

#define KERNEL_STRUCT_BEGIN(name) for (int array_index = 0;; array_index++) {
#define KERNEL_STRUCT_MEMBER(parent_struct, type, name) \
  { \
    device_only_memory<type> *array = new device_only_memory<type>(device_, \
                                                                   "integrator_state_" #name); \
    array->alloc_to_device(max_num_paths_); \
    integrator_state_soa_.emplace_back(array); \
    integrator_state_gpu_.parent_struct.name = (type *)array->device_pointer; \
  }
#define KERNEL_STRUCT_ARRAY_MEMBER(parent_struct, type, name) \
  { \
    device_only_memory<type> *array = new device_only_memory<type>(device_, \
                                                                   "integrator_state_" #name); \
    array->alloc_to_device(max_num_paths_); \
    integrator_state_soa_.emplace_back(array); \
    integrator_state_gpu_.parent_struct[array_index].name = (type *)array->device_pointer; \
  }
#define KERNEL_STRUCT_END(name) \
  break; \
  }
#define KERNEL_STRUCT_END_ARRAY(name, array_size) \
  if (array_index == array_size - 1) { \
    break; \
  } \
  }
#include "kernel/integrator/integrator_state_template.h"
#undef KERNEL_STRUCT_BEGIN
#undef KERNEL_STRUCT_MEMBER
#undef KERNEL_STRUCT_ARRAY_MEMBER
#undef KERNEL_STRUCT_END
#undef KERNEL_STRUCT_END_ARRAY
}

void PathTraceWorkGPU::alloc_integrator_queue()
{
  if (integrator_queue_counter_.size() == 0) {
    integrator_queue_counter_.alloc(1);
    integrator_queue_counter_.zero_to_device();
    integrator_queue_counter_.copy_from_device();
    integrator_state_gpu_.queue_counter = (IntegratorQueueCounter *)
                                              integrator_queue_counter_.device_pointer;
  }

  /* Allocate data for active path index arrays. */
  if (num_queued_paths_.size() == 0) {
    num_queued_paths_.alloc(1);
    num_queued_paths_.zero_to_device();
  }

  if (queued_paths_.size() == 0) {
    queued_paths_.alloc(max_num_paths_);
    /* TODO: this could be skip if we had a function to just allocate on device. */
    queued_paths_.zero_to_device();
  }
}

void PathTraceWorkGPU::alloc_integrator_sorting()
{
  /* Allocate arrays for shader sorting. */
  const int num_shaders = device_scene_->shaders.size();
  if (integrator_shader_sort_counter_.size() < num_shaders) {
    integrator_shader_sort_counter_.alloc(num_shaders);
    integrator_shader_sort_counter_.zero_to_device();

    integrator_shader_raytrace_sort_counter_.alloc(num_shaders);
    integrator_shader_raytrace_sort_counter_.zero_to_device();

    integrator_state_gpu_.sort_key_counter[DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE] =
        (int *)integrator_shader_sort_counter_.device_pointer;
    integrator_state_gpu_.sort_key_counter[DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE] =
        (int *)integrator_shader_raytrace_sort_counter_.device_pointer;
  }
}

void PathTraceWorkGPU::init_execution()
{
  queue_->init_execution();

  alloc_integrator_soa();
  alloc_integrator_queue();
  alloc_integrator_sorting();

  integrator_state_gpu_.shadow_catcher_state_offset = get_shadow_catcher_state_offset();

  /* Copy to device side struct in constant memory. */
  device_->const_copy_to(
      "__integrator_state", &integrator_state_gpu_, sizeof(integrator_state_gpu_));
}

void PathTraceWorkGPU::render_samples(int start_sample, int samples_num)
{
  /* Update number of available states based on the updated content of the scene (shadow catcher
   * object might have been added or removed). */
  work_tile_scheduler_.set_max_num_path_states(get_max_num_camera_paths());

  work_tile_scheduler_.reset(effective_buffer_params_, start_sample, samples_num);

  enqueue_reset();

  /* TODO: set a hard limit in case of undetected kernel failures? */
  while (true) {
    /* Enqueue work from the scheduler, on start or when there are not enough
     * paths to keep the device occupied. */
    bool finished;
    if (enqueue_work_tiles(finished)) {
      /* Copy stats from the device. */
      queue_->copy_from_device(integrator_queue_counter_);

      if (!queue_->synchronize()) {
        break; /* Stop on error. */
      }
    }

    if (is_cancel_requested()) {
      break;
    }

    /* Stop if no more work remaining. */
    if (finished) {
      break;
    }

    /* Enqueue on of the path iteration kernels. */
    if (enqueue_path_iteration()) {
      /* Copy stats from the device. */
      queue_->copy_from_device(integrator_queue_counter_);

      if (!queue_->synchronize()) {
        break; /* Stop on error. */
      }
    }

    if (is_cancel_requested()) {
      break;
    }
  }
}

DeviceKernel PathTraceWorkGPU::get_most_queued_kernel() const
{
  const IntegratorQueueCounter *queue_counter = integrator_queue_counter_.data();

  int max_num_queued = 0;
  DeviceKernel kernel = DEVICE_KERNEL_NUM;

  for (int i = 0; i < DEVICE_KERNEL_INTEGRATOR_NUM; i++) {
    if (queue_counter->num_queued[i] > max_num_queued) {
      kernel = (DeviceKernel)i;
      max_num_queued = queue_counter->num_queued[i];
    }
  }

  return kernel;
}

void PathTraceWorkGPU::enqueue_reset()
{
  void *args[] = {&max_num_paths_};
  queue_->enqueue(DEVICE_KERNEL_INTEGRATOR_RESET, max_num_paths_, args);
  queue_->zero_to_device(integrator_queue_counter_);
  queue_->zero_to_device(integrator_shader_sort_counter_);
  queue_->zero_to_device(integrator_shader_raytrace_sort_counter_);

  /* Tiles enqueue need to know number of active paths, which is based on this counter. Zero the
   * counter on the host side because `zero_to_device()` is not doing it. */
  if (integrator_queue_counter_.host_pointer) {
    memset(integrator_queue_counter_.data(), 0, integrator_queue_counter_.memory_size());
  }
}

bool PathTraceWorkGPU::enqueue_path_iteration()
{
  /* Find kernel to execute, with max number of queued paths. */
  const IntegratorQueueCounter *queue_counter = integrator_queue_counter_.data();

  int num_active_paths = 0;
  for (int i = 0; i < DEVICE_KERNEL_INTEGRATOR_NUM; i++) {
    num_active_paths += queue_counter->num_queued[i];
  }

  if (num_active_paths == 0) {
    return false;
  }

  /* Find kernel to execute, with max number of queued paths. */
  const DeviceKernel kernel = get_most_queued_kernel();
  if (kernel == DEVICE_KERNEL_NUM) {
    return false;
  }

  /* Finish shadows before potentially adding more shadow rays. We can only
   * store one shadow ray in the integrator state. */
  if (kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE ||
      kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE ||
      kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME) {
    if (queue_counter->num_queued[DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW]) {
      enqueue_path_iteration(DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW);
      return true;
    }
    else if (queue_counter->num_queued[DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW]) {
      enqueue_path_iteration(DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW);
      return true;
    }
  }

  /* Schedule kernel with maximum number of queued items. */
  enqueue_path_iteration(kernel);
  return true;
}

void PathTraceWorkGPU::enqueue_path_iteration(DeviceKernel kernel)
{
  void *d_path_index = (void *)NULL;

  /* Create array of path indices for which this kernel is queued to be executed. */
  int work_size = max_active_path_index_;

  IntegratorQueueCounter *queue_counter = integrator_queue_counter_.data();
  int num_queued = queue_counter->num_queued[kernel];

  if (kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE ||
      kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE) {
    /* Compute array of active paths, sorted by shader. */
    work_size = num_queued;
    d_path_index = (void *)queued_paths_.device_pointer;

    compute_sorted_queued_paths(DEVICE_KERNEL_INTEGRATOR_SORTED_PATHS_ARRAY, kernel);
  }
  else if (num_queued < work_size) {
    work_size = num_queued;
    d_path_index = (void *)queued_paths_.device_pointer;

    if (kernel == DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW ||
        kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW) {
      /* Compute array of active shadow paths for specific kernel. */
      compute_queued_paths(DEVICE_KERNEL_INTEGRATOR_QUEUED_SHADOW_PATHS_ARRAY, kernel);
    }
    else {
      /* Compute array of active paths for specific kernel. */
      compute_queued_paths(DEVICE_KERNEL_INTEGRATOR_QUEUED_PATHS_ARRAY, kernel);
    }
  }

  DCHECK_LE(work_size, max_num_paths_);

  switch (kernel) {
    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST:
    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW:
    case DEVICE_KERNEL_INTEGRATOR_INTERSECT_SUBSURFACE: {
      /* Ray intersection kernels with integrator state. */
      void *args[] = {&d_path_index, const_cast<int *>(&work_size)};

      queue_->enqueue(kernel, work_size, args);
      break;
    }
    case DEVICE_KERNEL_INTEGRATOR_SHADE_BACKGROUND:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_LIGHT:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE:
    case DEVICE_KERNEL_INTEGRATOR_SHADE_VOLUME: {
      /* Shading kernels with integrator state and render buffer. */
      void *d_render_buffer = (void *)render_buffers_->buffer.device_pointer;
      void *args[] = {&d_path_index, &d_render_buffer, const_cast<int *>(&work_size)};

      queue_->enqueue(kernel, work_size, args);
      break;
    }
    case DEVICE_KERNEL_INTEGRATOR_INIT_FROM_CAMERA:
    case DEVICE_KERNEL_INTEGRATOR_INIT_FROM_BAKE:
    case DEVICE_KERNEL_INTEGRATOR_MEGAKERNEL:
    case DEVICE_KERNEL_INTEGRATOR_QUEUED_PATHS_ARRAY:
    case DEVICE_KERNEL_INTEGRATOR_QUEUED_SHADOW_PATHS_ARRAY:
    case DEVICE_KERNEL_INTEGRATOR_ACTIVE_PATHS_ARRAY:
    case DEVICE_KERNEL_INTEGRATOR_TERMINATED_PATHS_ARRAY:
    case DEVICE_KERNEL_INTEGRATOR_SORTED_PATHS_ARRAY:
    case DEVICE_KERNEL_INTEGRATOR_COMPACT_PATHS_ARRAY:
    case DEVICE_KERNEL_INTEGRATOR_COMPACT_STATES:
    case DEVICE_KERNEL_INTEGRATOR_RESET:
    case DEVICE_KERNEL_SHADER_EVAL_DISPLACE:
    case DEVICE_KERNEL_SHADER_EVAL_BACKGROUND:
    case DEVICE_KERNEL_FILM_CONVERT_DEPTH_HALF_RGBA:
    case DEVICE_KERNEL_FILM_CONVERT_MIST_HALF_RGBA:
    case DEVICE_KERNEL_FILM_CONVERT_SAMPLE_COUNT_HALF_RGBA:
    case DEVICE_KERNEL_FILM_CONVERT_FLOAT_HALF_RGBA:
    case DEVICE_KERNEL_FILM_CONVERT_SHADOW3_HALF_RGBA:
    case DEVICE_KERNEL_FILM_CONVERT_DIVIDE_EVEN_COLOR_HALF_RGBA:
    case DEVICE_KERNEL_FILM_CONVERT_FLOAT3_HALF_RGBA:
    case DEVICE_KERNEL_FILM_CONVERT_SHADOW4_HALF_RGBA:
    case DEVICE_KERNEL_FILM_CONVERT_MOTION_HALF_RGBA:
    case DEVICE_KERNEL_FILM_CONVERT_CRYPTOMATTE_HALF_RGBA:
    case DEVICE_KERNEL_FILM_CONVERT_DENOISING_COLOR_HALF_RGBA:
    case DEVICE_KERNEL_FILM_CONVERT_SHADOW_CATCHER_HALF_RGBA:
    case DEVICE_KERNEL_FILM_CONVERT_SHADOW_CATCHER_MATTE_WITH_SHADOW_HALF_RGBA:
    case DEVICE_KERNEL_FILM_CONVERT_FLOAT4_HALF_RGBA:
    case DEVICE_KERNEL_ADAPTIVE_SAMPLING_CONVERGENCE_CHECK:
    case DEVICE_KERNEL_ADAPTIVE_SAMPLING_CONVERGENCE_FILTER_X:
    case DEVICE_KERNEL_ADAPTIVE_SAMPLING_CONVERGENCE_FILTER_Y:
    case DEVICE_KERNEL_FILTER_CONVERT_TO_RGB:
    case DEVICE_KERNEL_FILTER_CONVERT_FROM_RGB:
    case DEVICE_KERNEL_PREFIX_SUM:
    case DEVICE_KERNEL_NUM: {
      LOG(FATAL) << "Unhandled kernel " << kernel << ", should never happen.";
      break;
    }
  }
}

void PathTraceWorkGPU::compute_sorted_queued_paths(DeviceKernel kernel, DeviceKernel queued_kernel)
{
  int d_queued_kernel = queued_kernel;
  void *d_counter = integrator_state_gpu_.sort_key_counter[d_queued_kernel];
  assert(d_counter != nullptr);

  /* Compute prefix sum of number of active paths with each shader. */
  {
    const int work_size = 1;
    int num_shaders = device_scene_->shaders.size();
    void *args[] = {&d_counter, &num_shaders};
    queue_->enqueue(DEVICE_KERNEL_PREFIX_SUM, work_size, args);
  }

  queue_->zero_to_device(num_queued_paths_);

  /* Launch kernel to fill the active paths arrays. */
  {
    /* TODO: this could be smaller for terminated paths based on amount of work we want
     * to schedule. */
    const int work_size = max_active_path_index_;

    void *d_queued_paths = (void *)queued_paths_.device_pointer;
    void *d_num_queued_paths = (void *)num_queued_paths_.device_pointer;
    void *args[] = {const_cast<int *>(&work_size),
                    &d_queued_paths,
                    &d_num_queued_paths,
                    &d_counter,
                    &d_queued_kernel};

    queue_->enqueue(kernel, work_size, args);
  }

  if (queued_kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE) {
    queue_->zero_to_device(integrator_shader_sort_counter_);
  }
  else if (queued_kernel == DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE) {
    queue_->zero_to_device(integrator_shader_raytrace_sort_counter_);
  }
  else {
    assert(0);
  }
}

void PathTraceWorkGPU::compute_queued_paths(DeviceKernel kernel, DeviceKernel queued_kernel)
{
  int d_queued_kernel = queued_kernel;

  /* Launch kernel to fill the active paths arrays. */
  const int work_size = max_active_path_index_;
  void *d_queued_paths = (void *)queued_paths_.device_pointer;
  void *d_num_queued_paths = (void *)num_queued_paths_.device_pointer;
  void *args[] = {
      const_cast<int *>(&work_size), &d_queued_paths, &d_num_queued_paths, &d_queued_kernel};

  queue_->zero_to_device(num_queued_paths_);
  queue_->enqueue(kernel, work_size, args);
}

void PathTraceWorkGPU::compact_states(const int num_active_paths)
{
  if (num_active_paths == 0) {
    max_active_path_index_ = 0;
  }

  /* TODO: not supported for shadow catcher yet. That needs to switch to an atomic
   * counter for new paths so that we can fill in the space left after compaction. */
  if (has_shadow_catcher()) {
    return;
  }

  /* Compact fragmented path states into the start of the array, moving any paths
   * with index higher than the number of active paths into the gaps. */
  if (max_active_path_index_ == num_active_paths) {
    return;
  }

  void *d_compact_paths = (void *)queued_paths_.device_pointer;
  void *d_num_queued_paths = (void *)num_queued_paths_.device_pointer;

  /* Create array with terminated paths that we can write to. */
  {
    /* TODO: can the work size be reduced here? */
    int offset = num_active_paths;
    int work_size = num_active_paths;
    void *args[] = {&work_size, &d_compact_paths, &d_num_queued_paths, &offset};
    queue_->zero_to_device(num_queued_paths_);
    queue_->enqueue(DEVICE_KERNEL_INTEGRATOR_TERMINATED_PATHS_ARRAY, work_size, args);
  }

  /* Create array of paths that we need to compact, where the path index is bigger
   * than the number of active paths. */
  {
    int work_size = max_active_path_index_;
    void *args[] = {
        &work_size, &d_compact_paths, &d_num_queued_paths, const_cast<int *>(&num_active_paths)};
    queue_->zero_to_device(num_queued_paths_);
    queue_->enqueue(DEVICE_KERNEL_INTEGRATOR_COMPACT_PATHS_ARRAY, work_size, args);
  }

  queue_->copy_from_device(num_queued_paths_);
  queue_->synchronize();

  int num_compact_paths = num_queued_paths_.data()[0];

  /* Move paths into gaps. */
  if (num_compact_paths > 0) {
    int work_size = num_compact_paths;
    int active_states_offset = 0;
    int terminated_states_offset = num_active_paths;
    void *args[] = {
        &d_compact_paths, &active_states_offset, &terminated_states_offset, &work_size};
    queue_->enqueue(DEVICE_KERNEL_INTEGRATOR_COMPACT_STATES, work_size, args);
  }

  queue_->synchronize();

  /* Adjust max active path index now we know which part of the array is actually used. */
  max_active_path_index_ = num_active_paths;
}

bool PathTraceWorkGPU::enqueue_work_tiles(bool &finished)
{
  /* If there are existing paths wait them to go to intersect closest kernel, which will align the
   * wavefront of the existing and newely added paths. */
  /* TODO: Check whether counting new intersection kernels here will have positive affect on the
   * performance. */
  const DeviceKernel kernel = get_most_queued_kernel();
  if (kernel != DEVICE_KERNEL_NUM && kernel != DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST) {
    return false;
  }

  int num_active_paths = get_num_active_paths();

  /* Don't schedule more work if cancelling. */
  if (is_cancel_requested()) {
    if (num_active_paths == 0) {
      finished = true;
    }
    return false;
  }

  finished = false;

  vector<KernelWorkTile> work_tiles;

  const int max_num_camera_paths = get_max_num_camera_paths();

  /* Schedule when we're out of paths or there are too few paths to keep the
   * device occupied. */
  int num_paths = num_active_paths;
  if (num_paths == 0 || num_paths < min_num_active_paths_) {
    /* Get work tiles until the maximum number of path is reached. */
    while (num_paths < max_num_camera_paths) {
      KernelWorkTile work_tile;
      if (work_tile_scheduler_.get_work(&work_tile, max_num_camera_paths - num_paths)) {
        work_tiles.push_back(work_tile);
        num_paths += work_tile.w * work_tile.h * work_tile.num_samples;
      }
      else {
        break;
      }
    }

    /* If we couldn't get any more tiles, we're done. */
    if (work_tiles.size() == 0 && num_paths == 0) {
      finished = true;
      return false;
    }
  }

  /* Initialize paths from work tiles. */
  if (work_tiles.size() == 0) {
    return false;
  }

  /* Compact state array when number of paths becomes small relative to the
   * known maximum path index, which makes computing active index arrays slow. */
  compact_states(num_active_paths);

  enqueue_work_tiles((device_scene_->data.bake.use) ? DEVICE_KERNEL_INTEGRATOR_INIT_FROM_BAKE :
                                                      DEVICE_KERNEL_INTEGRATOR_INIT_FROM_CAMERA,
                     work_tiles.data(),
                     work_tiles.size());
  return true;
}

void PathTraceWorkGPU::enqueue_work_tiles(DeviceKernel kernel,
                                          const KernelWorkTile work_tiles[],
                                          const int num_work_tiles)
{
  /* Copy work tiles to device. */
  if (work_tiles_.size() < num_work_tiles) {
    work_tiles_.alloc(num_work_tiles);
  }

  int path_index_offset = 0;
  int max_tile_work_size = 0;
  for (int i = 0; i < num_work_tiles; i++) {
    KernelWorkTile &work_tile = work_tiles_.data()[i];
    work_tile = work_tiles[i];

    const int tile_work_size = work_tile.w * work_tile.h * work_tile.num_samples;

    work_tile.path_index_offset = path_index_offset;
    work_tile.work_size = tile_work_size;

    path_index_offset += tile_work_size;

    max_tile_work_size = max(max_tile_work_size, tile_work_size);
  }

  queue_->copy_to_device(work_tiles_);

  void *d_work_tiles = (void *)work_tiles_.device_pointer;
  void *d_path_index = (void *)nullptr;
  void *d_render_buffer = (void *)render_buffers_->buffer.device_pointer;

  if (max_active_path_index_ != 0) {
    queue_->zero_to_device(num_queued_paths_);

    /* Limit work size to max known active path index + the number of paths we are going
     * to enqueue, which may be smaller than the total number of paths possible. */
    const int work_size = min(max_num_paths_, max_active_path_index_ + path_index_offset);
    int queued_kernel = 0;

    void *d_queued_paths = (void *)queued_paths_.device_pointer;
    void *d_num_queued_paths = (void *)num_queued_paths_.device_pointer;
    void *args[] = {
        const_cast<int *>(&work_size), &d_queued_paths, &d_num_queued_paths, &queued_kernel};

    queue_->enqueue(DEVICE_KERNEL_INTEGRATOR_TERMINATED_PATHS_ARRAY, work_size, args);

    d_path_index = (void *)queued_paths_.device_pointer;
  }

  /* Launch kernel. */
  void *args[] = {&d_path_index,
                  &d_work_tiles,
                  const_cast<int *>(&num_work_tiles),
                  &d_render_buffer,
                  const_cast<int *>(&max_tile_work_size)};

  queue_->enqueue(kernel, max_tile_work_size * num_work_tiles, args);

  /* TODO: this could be computed more accurately using on the last entry
   * in the queued_paths array passed to the kernel? */
  /* When there is a shadow catcher in the scene provision that the shadow catcher state will
   * become active at some point.
   *
   * TODO: What is more accurate approach here? What if the shadow catcher is hit after some
   * transparent bounce? Do we need to calculate this somewhere else as well? */
  max_active_path_index_ = min(max_active_path_index_ + path_index_offset +
                                   get_shadow_catcher_state_offset(),
                               max_num_paths_);
}

int PathTraceWorkGPU::get_num_active_paths()
{
  /* TODO: this is wrong, does not account for duplicates with shadow! */
  IntegratorQueueCounter *queue_counter = integrator_queue_counter_.data();

  int num_paths = 0;
  for (int i = 0; i < DEVICE_KERNEL_INTEGRATOR_NUM; i++) {
    DCHECK_GE(queue_counter->num_queued[i], 0)
        << "Invalid number of queued states for kernel "
        << device_kernel_as_string(static_cast<DeviceKernel>(i));
    num_paths += queue_counter->num_queued[i];
  }

  return num_paths;
}

int PathTraceWorkGPU::get_max_num_camera_paths() const
{
  /* When shadow catcher is used reserve half of the states for the shadow catcher needs (so that
   * when path hits shadow catcher it can split). */
  if (has_shadow_catcher()) {
    return max_num_paths_ / 2;
  }

  return max_num_paths_;
}

void PathTraceWorkGPU::copy_to_gpu_display(GPUDisplay *gpu_display, int num_samples)
{
  if (!interop_use_checked_) {
    Device *device = queue_->device;
    interop_use_ = device->should_use_graphics_interop();

    if (interop_use_) {
      VLOG(2) << "Will be using graphics interop GPU display update.";
    }
    else {
      VLOG(2) << "Will be using naive GPU display update.";
    }

    interop_use_checked_ = true;
  }

  if (interop_use_) {
    if (copy_to_gpu_display_interop(gpu_display, num_samples)) {
      return;
    }
    interop_use_ = false;
  }

  copy_to_gpu_display_naive(gpu_display, num_samples);
}

void PathTraceWorkGPU::copy_to_gpu_display_naive(GPUDisplay *gpu_display, int num_samples)
{
  const int width = effective_buffer_params_.width;
  const int height = effective_buffer_params_.height;
  const int final_width = render_buffers_->params.width;
  const int final_height = render_buffers_->params.height;

  /* Re-allocate display memory if needed, and make sure the device pointer is allocated.
   *
   * NOTE: allocation happens to the final resolution so that no re-allocation happens on every
   * change of the resolution divider. However, if the display becomes smaller, shrink the
   * allocated memory as well. */
  if (gpu_display_rgba_half_.data_width != final_width ||
      gpu_display_rgba_half_.data_height != final_height) {
    gpu_display_rgba_half_.alloc(width, height);
    /* TODO(sergey): There should be a way to make sure device-side memory is allocated without
     * transfering zeroes to the device. */
    queue_->zero_to_device(gpu_display_rgba_half_);
  }

  run_film_convert(gpu_display_rgba_half_.device_pointer, num_samples);

  gpu_display_rgba_half_.copy_from_device();

  gpu_display->copy_pixels_to_texture(gpu_display_rgba_half_.data());
}

bool PathTraceWorkGPU::copy_to_gpu_display_interop(GPUDisplay *gpu_display, int num_samples)
{
  Device *device = queue_->device;

  if (!device_graphics_interop_) {
    device_graphics_interop_ = device->graphics_interop_create();
  }

  const DeviceGraphicsInteropDestination graphics_interop_dst =
      gpu_display->graphics_interop_get();
  device_graphics_interop_->set_destination(graphics_interop_dst);

  const device_ptr d_rgba_half = device_graphics_interop_->map();
  if (!d_rgba_half) {
    return false;
  }

  run_film_convert(d_rgba_half, num_samples);

  device_graphics_interop_->unmap();

  return true;
}

void PathTraceWorkGPU::run_film_convert(device_ptr d_rgba_half, int num_samples)
{
  const KernelFilm &kfilm = device_scene_->data.film;

  /* TODO(sergey): De-duplicate with `PathTraceWorkCPU`. */
  PassAccessor::PassAccessInfo pass_access_info;
  pass_access_info.type = static_cast<PassType>(kfilm.display_pass_type);
  pass_access_info.offset = kfilm.display_pass_offset;
  pass_access_info.use_approximate_shadow_catcher = kfilm.use_approximate_shadow_catcher;
  pass_access_info.show_active_pixels = kfilm.show_active_pixels;

  const PassAccessorGPU pass_accessor(queue_.get(), pass_access_info, kfilm.exposure, num_samples);

  PassAccessor::Destination destination(pass_access_info.type);
  destination.d_pixels_half_rgba = d_rgba_half;

  pass_accessor.get_render_tile_pixels(render_buffers_, effective_buffer_params_, destination);
}

int PathTraceWorkGPU::adaptive_sampling_converge_filter_count_active(float threshold, bool reset)
{
  const int num_active_pixels = adaptive_sampling_convergence_check_count_active(threshold, reset);

  if (num_active_pixels) {
    enqueue_adaptive_sampling_filter_x();
    enqueue_adaptive_sampling_filter_y();
    queue_->synchronize();
  }

  return num_active_pixels;
}

int PathTraceWorkGPU::adaptive_sampling_convergence_check_count_active(float threshold, bool reset)
{
  device_vector<uint> num_active_pixels(device_, "num_active_pixels", MEM_READ_WRITE);
  num_active_pixels.alloc(1);

  queue_->zero_to_device(num_active_pixels);

  const int work_size = effective_buffer_params_.width * effective_buffer_params_.height;

  void *args[] = {&render_buffers_->buffer.device_pointer,
                  const_cast<int *>(&effective_buffer_params_.full_x),
                  const_cast<int *>(&effective_buffer_params_.full_y),
                  const_cast<int *>(&effective_buffer_params_.width),
                  const_cast<int *>(&effective_buffer_params_.height),
                  &threshold,
                  &reset,
                  &effective_buffer_params_.offset,
                  &effective_buffer_params_.stride,
                  &num_active_pixels.device_pointer};

  queue_->enqueue(DEVICE_KERNEL_ADAPTIVE_SAMPLING_CONVERGENCE_CHECK, work_size, args);

  queue_->copy_from_device(num_active_pixels);
  queue_->synchronize();

  return num_active_pixels.data()[0];
}

void PathTraceWorkGPU::enqueue_adaptive_sampling_filter_x()
{
  const int work_size = effective_buffer_params_.height;

  void *args[] = {&render_buffers_->buffer.device_pointer,
                  &effective_buffer_params_.full_x,
                  &effective_buffer_params_.full_y,
                  &effective_buffer_params_.width,
                  &effective_buffer_params_.height,
                  &effective_buffer_params_.offset,
                  &effective_buffer_params_.stride};

  queue_->enqueue(DEVICE_KERNEL_ADAPTIVE_SAMPLING_CONVERGENCE_FILTER_X, work_size, args);
}

void PathTraceWorkGPU::enqueue_adaptive_sampling_filter_y()
{
  const int work_size = effective_buffer_params_.width;

  void *args[] = {&render_buffers_->buffer.device_pointer,
                  &effective_buffer_params_.full_x,
                  &effective_buffer_params_.full_y,
                  &effective_buffer_params_.width,
                  &effective_buffer_params_.height,
                  &effective_buffer_params_.offset,
                  &effective_buffer_params_.stride};

  queue_->enqueue(DEVICE_KERNEL_ADAPTIVE_SAMPLING_CONVERGENCE_FILTER_Y, work_size, args);
}

bool PathTraceWorkGPU::has_shadow_catcher() const
{
  return device_scene_->data.integrator.has_shadow_catcher;
}

int PathTraceWorkGPU::get_shadow_catcher_state_offset() const
{
  if (!has_shadow_catcher()) {
    return 0;
  }

  return max_num_paths_ / 2;
}

CCL_NAMESPACE_END
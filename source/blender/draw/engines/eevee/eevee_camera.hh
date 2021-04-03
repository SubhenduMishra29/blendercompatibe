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

/** \file
 * \ingroup eevee
 */

#pragma once

#include <array>

#include "DRW_render.h"

#include "BKE_camera.h"

#include "RE_pipeline.h"

#include "DNA_camera_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"

#include "eevee_sampling.hh"
#include "eevee_shader_shared.hh"

namespace blender::eevee {

/* TODO(fclem) Might want to move to eevee_shader_shared.hh. */
static const float cubeface_mat[6][4][4] = {
    /* Pos X */
    {{0.0f, 0.0f, -1.0f, 0.0f},
     {0.0f, -1.0f, 0.0f, 0.0f},
     {-1.0f, 0.0f, 0.0f, 0.0f},
     {0.0f, 0.0f, 0.0f, 1.0f}},
    /* Neg X */
    {{0.0f, 0.0f, 1.0f, 0.0f},
     {0.0f, -1.0f, 0.0f, 0.0f},
     {1.0f, 0.0f, 0.0f, 0.0f},
     {0.0f, 0.0f, 0.0f, 1.0f}},
    /* Pos Y */
    {{1.0f, 0.0f, 0.0f, 0.0f},
     {0.0f, 0.0f, -1.0f, 0.0f},
     {0.0f, 1.0f, 0.0f, 0.0f},
     {0.0f, 0.0f, 0.0f, 1.0f}},
    /* Neg Y */
    {{1.0f, 0.0f, 0.0f, 0.0f},
     {0.0f, 0.0f, 1.0f, 0.0f},
     {0.0f, -1.0f, 0.0f, 0.0f},
     {0.0f, 0.0f, 0.0f, 1.0f}},
    /* Pos Z */
    {{1.0f, 0.0f, 0.0f, 0.0f},
     {0.0f, -1.0f, 0.0f, 0.0f},
     {0.0f, 0.0f, -1.0f, 0.0f},
     {0.0f, 0.0f, 0.0f, 1.0f}},
    /* Neg Z */
    {{-1.0f, 0.0f, 0.0f, 0.0f},
     {0.0f, -1.0f, 0.0f, 0.0f},
     {0.0f, 0.0f, 1.0f, 0.0f},
     {0.0f, 0.0f, 0.0f, 1.0f}},
};

/* -------------------------------------------------------------------- */
/** \name eCameraType
 * \{ */

static eCameraType from_camera(const ::Camera *camera)
{
  switch (camera->type) {
    default:
    case CAM_PERSP:
      return CAMERA_PERSP;
    case CAM_ORTHO:
      return CAMERA_ORTHO;
    case CAM_PANO:
      switch (camera->panorama_type) {
        default:
        case CAM_PANO_EQUIRECTANGULAR:
          return CAMERA_PANO_EQUIRECT;
        case CAM_PANO_FISHEYE_EQUIDISTANT:
          return CAMERA_PANO_EQUIDISTANT;
        case CAM_PANO_FISHEYE_EQUISOLID:
          return CAMERA_PANO_EQUISOLID;
        case CAM_PANO_MIRRORBALL:
          return CAMERA_PANO_MIRROR;
      }
  }
}

static bool is_panoramic(eCameraType type)
{
  return !ELEM(type, CAMERA_PERSP, CAMERA_ORTHO);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name CameraData operators
 * \{ */

inline bool operator==(const CameraData &a, const CameraData &b)
{
  return compare_m4m4(a.persmat, b.persmat, FLT_MIN) && equals_v2v2(a.uv_scale, b.uv_scale) &&
         equals_v2v2(a.uv_bias, b.uv_bias) && equals_v2v2(a.equirect_scale, b.equirect_scale) &&
         equals_v2v2(a.equirect_bias, b.equirect_bias) && (a.fisheye_fov == b.fisheye_fov) &&
         (a.fisheye_lens == b.fisheye_lens) && (a.filter_size == b.filter_size) &&
         (a.type == b.type);
}

inline bool operator!=(const CameraData &a, const CameraData &b)
{
  return !(a == b);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Camera
 * \{ */

class CameraView {
  friend class Camera;

 private:
  /** Main views is created from the camera (or is from the viewport). It is not jittered. */
  DRWView *main_view_ = nullptr;
  /** Sub views is jittered versions or the main views. This allows jitter updates without trashing
   * the visibility culling cache. */
  DRWView *sub_view_ = nullptr;
  /** Render size of the view. Can change between scene sample eval. */
  int extent_[2] = {-1, -1};
  /** Static srting pointer. Used as debug name and as UUID for texture pool. */
  const char *name_;
  /** Matrix to apply to the viewmat. */
  const float (*face_matrix_)[4];

 public:
  bool is_enabled() const
  {
    return main_view_ != nullptr;
  }
  const DRWView *drw_view_get(void) const
  {
    BLI_assert(this->is_enabled());
    return sub_view_;
  }
  const char *name_get(void) const
  {
    return name_;
  }
  const int *extent_get(void) const
  {
    BLI_assert(this->is_enabled());
    return extent_;
  }

 private:
  CameraView(const char *name, const float (*face_matrix)[4])
      : name_(name), face_matrix_(face_matrix){};

  void sync(const CameraData &data, const int extent[2])
  {
    copy_v2_v2_int(extent_, extent);

    float viewmat[4][4], winmat[4][4];
    const float(*viewmat_p)[4] = viewmat, (*winmat_p)[4] = winmat;
    if (is_panoramic(data.type)) {
      /* TODO(fclem) Overscans. */
      float near = data.clip_near;
      float far = data.clip_far;
      perspective_m4(winmat, -near, near, -near, near, near, far);
      mul_m4_m4m4(viewmat, face_matrix_, data.viewmat);
    }
    else {
      viewmat_p = data.viewmat;
      winmat_p = data.winmat;
    }

    main_view_ = DRW_view_create(viewmat_p, winmat_p, nullptr, nullptr, nullptr);
    sub_view_ = DRW_view_create_sub(main_view_, viewmat_p, winmat_p);
  }

  void update(Sampling &sampling)
  {
    if (!this->is_enabled()) {
      return;
    }

    float viewmat[4][4], winmat[4][4], persmat[4][4];
    DRW_view_viewmat_get(main_view_, viewmat, false);
    DRW_view_winmat_get(main_view_, winmat, false);
    DRW_view_persmat_get(main_view_, persmat, false);

    /* Apply jitter. */
    float jitter[2];
    sampling.camera_lds_get(jitter);
    for (int i = 0; i < 2; i++) {
      jitter[i] = 2.0f * (jitter[i] - 0.5f) / extent_[i];
    }

    window_translate_m4(winmat, persmat, UNPACK2(jitter));

    DRW_view_update_sub(sub_view_, viewmat, winmat);
  }

  void disable(void)
  {
    main_view_ = nullptr;
  }
};

class Camera {
  friend CameraView;

 private:
  eCameraType type_;
  /** Splitted portions of the camera FOV that can be rendered. */
  std::array<CameraView, 6> views_ = {CameraView("posX_view", cubeface_mat[0]),
                                      CameraView("negX_view", cubeface_mat[1]),
                                      CameraView("posY_view", cubeface_mat[2]),
                                      CameraView("negY_view", cubeface_mat[3]),
                                      CameraView("posZ_view", cubeface_mat[4]),
                                      CameraView("negZ_view", cubeface_mat[5])};
  /** Random module to know what jitter to apply to the view. */
  Sampling &sampling_;
  /** Double buffered to detect changes and have history for re-projection. */
  struct {
    CameraData *data_;
    GPUUniformBuf *ubo_;
  } current, previous;
  /** Render size of the whole image. */
  int full_extent_[2];
  /** Internal render size. */
  int render_extent_[2];
  /** True if camera matrix has change since last init. */
  bool has_changed_ = true;
  /** Detects wrong usage. */
  bool synced_ = false;
  /** Last sample we synced with. Avoid double sync. */
  uint64_t last_sample_ = 0;

 public:
  Camera(Sampling &sampling) : sampling_(sampling)
  {
    current.data_ = (CameraData *)MEM_callocN(sizeof(CameraData), "CameraData");
    current.ubo_ = GPU_uniformbuf_create_ex(sizeof(CameraData), nullptr, "CameraData");

    previous.data_ = (CameraData *)MEM_callocN(sizeof(CameraData), "CameraData");
    previous.ubo_ = GPU_uniformbuf_create_ex(sizeof(CameraData), nullptr, "CameraData");
  };

  ~Camera()
  {
    MEM_SAFE_FREE(current.data_);
    MEM_SAFE_FREE(previous.data_);
    DRW_UBO_FREE_SAFE(current.ubo_);
    DRW_UBO_FREE_SAFE(previous.ubo_);
  };

  void init(const RenderEngine *engine,
            const Object *camera_object_eval,
            const DRWView *drw_view,
            const Scene *scene,
            const int full_extent[2])
  {
    synced_ = false;
    copy_v2_v2_int(full_extent_, full_extent);

    SWAP(CameraData *, current.data_, previous.data_);
    SWAP(GPUUniformBuf *, current.ubo_, previous.ubo_);

    CameraData &data = *current.data_;

    if (camera_object_eval) {
      const ::Camera *cam = reinterpret_cast<const ::Camera *>(camera_object_eval->data);
      data.type = from_camera(cam);
    }
    else {
      data.type = DRW_view_is_persp_get(drw_view) ? CAMERA_PERSP : CAMERA_ORTHO;
    }

    /* Sync early to detect changes. This is ok since we avoid double sync later. */
    this->sync(engine, camera_object_eval, drw_view, scene);

    /* Detect changes in parameters. */
    has_changed_ = *current.data_ != *previous.data_;
    if (has_changed_) {
      sampling_.reset();
    }
  }

  void sync(const RenderEngine *engine,
            const Object *camera_object_eval,
            const DRWView *drw_view,
            const Scene *scene)
  {
    uint64_t sample = sampling_.sample_get();
    if (last_sample_ != sample || !synced_) {
      last_sample_ = sample;
    }
    else {
      /* Avoid double sync. */
      return;
    }

    CameraData &data = *current.data_;

    data.filter_size = scene->r.gauss;

    if (drw_view) {
      DRW_view_viewmat_get(drw_view, data.viewmat, false);
      DRW_view_viewmat_get(drw_view, data.viewinv, true);
      DRW_view_winmat_get(drw_view, data.winmat, false);
      DRW_view_winmat_get(drw_view, data.wininv, true);
      DRW_view_persmat_get(drw_view, data.persmat, false);
      DRW_view_persmat_get(drw_view, data.persinv, true);
      DRW_view_camtexco_get(drw_view, &data.uv_scale[0]);
    }
    else if (engine) {
      /* TODO(fclem) Overscan */
      // RE_GetCameraWindowWithOverscan(engine->re, g_data->overscan, data.winmat);
      RE_GetCameraWindow(engine->re, camera_object_eval, data.winmat);
      RE_GetCameraModelMatrix(engine->re, camera_object_eval, data.viewinv);
      invert_m4_m4(data.viewmat, data.viewinv);
      invert_m4_m4(data.wininv, data.winmat);
      mul_m4_m4m4(data.persmat, data.winmat, data.viewmat);
      invert_m4_m4(data.persinv, data.persmat);
      copy_v2_fl(data.uv_scale, 1.0f);
      copy_v2_fl(data.uv_bias, 0.0f);
    }
    else {
      BLI_assert(0);
    }

    if (camera_object_eval) {
      const ::Camera *cam = reinterpret_cast<const ::Camera *>(camera_object_eval->data);
      data.clip_near = cam->clip_start;
      data.clip_far = cam->clip_end;
      data.fisheye_fov = cam->fisheye_fov;
      data.fisheye_lens = cam->fisheye_lens;
      data.equirect_bias[0] = -cam->longitude_min + M_PI_2;
      data.equirect_bias[1] = -cam->latitude_min + M_PI_2;
      data.equirect_scale[0] = cam->longitude_min - cam->longitude_max;
      data.equirect_scale[1] = cam->latitude_min - cam->latitude_max;
      /* Combine with uv_scale/bias to avoid doing extra computation. */
      madd_v2_v2v2(data.equirect_bias, data.uv_bias, data.equirect_scale);
      mul_v2_v2(data.equirect_scale, data.uv_scale);

      copy_v2_v2(data.equirect_scale_inv, data.equirect_scale);
      invert_v2(data.equirect_scale_inv);
    }
    else {
      data.clip_near = DRW_view_near_distance_get(drw_view);
      data.clip_far = DRW_view_far_distance_get(drw_view);
      data.fisheye_fov = data.fisheye_lens = -1.0f;
      copy_v2_fl(data.equirect_bias, 0.0f);
      copy_v2_fl(data.equirect_scale, 0.0f);
    }

    /* TODO(fclem) parameter hidden in experimental. We need to figure out mipmap bias to preserve
     * texture crispiness. */
    float resolution_scale = 1.0f;
    for (int i = 0; i < 2; i++) {
      render_extent_[i] = max_ii(1, roundf(full_extent_[i] * resolution_scale));
    }

    for (CameraView &view : views_) {
      view.disable();
    }

    if (this->is_panoramic()) {
      int extent[2];
      int64_t render_pixel_count = render_extent_[0] * (int64_t)render_extent_[1];
      /* Divide pixel count between the 6 views. Rendering to a square target. */
      extent[0] = extent[1] = ceilf(sqrtf(1 + (render_pixel_count / 6)));

      /* TODO(fclem) Clip unused views heres. */
      for (CameraView &view : views_) {
        view.sync(*current.data_, extent);
      }
    }
    else {
      /* Only enable -Z view. */
      views_[5].sync(*current.data_, render_extent_);
    }

    GPU_uniformbuf_update(current.ubo_, current.data_);

    synced_ = true;
  }

  /* Update views for new sample. */
  void update_views(void)
  {
    BLI_assert(synced_);
    for (CameraView &view : views_) {
      view.update(sampling_);
    }
  }

  /**
   * Getters
   **/
  const auto &views_get(void) const
  {
    return views_;
  }
  const CameraData &data_get(void) const
  {
    BLI_assert(synced_);
    return *current.data_;
  }
  const GPUUniformBuf *ubo_get(void) const
  {
    return current.ubo_;
  }
  bool has_changed(void) const
  {
    BLI_assert(synced_);
    return has_changed_;
  }
  bool is_panoramic(void) const
  {
    return eevee::is_panoramic(current.data_->type);
  }
};

/** \} */

}  // namespace blender::eevee
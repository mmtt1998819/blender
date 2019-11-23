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
 * Copyright 2019, Blender Foundation.
 */

/** \file
 * \ingroup draw_engine
 */

#include "DNA_mesh_types.h"
#include "DNA_view3d_types.h"

#include "BKE_editmesh.h"
#include "BKE_global.h"
#include "BKE_object.h"
#include "BKE_paint.h"

#include "BLI_hash.h"

#include "GPU_shader.h"
#include "DRW_render.h"

#include "ED_view3d.h"

#include "overlay_private.h"

void OVERLAY_wireframe_init(OVERLAY_Data *vedata)
{
  OVERLAY_PrivateData *pd = vedata->stl->pd;
  const DRWContextState *draw_ctx = DRW_context_state_get();
  pd->view_wires = DRW_view_create_with_zoffset(pd->view_default, draw_ctx->rv3d, 0.5f);
}

void OVERLAY_wireframe_cache_init(OVERLAY_Data *vedata)
{
  OVERLAY_PassList *psl = vedata->psl;
  OVERLAY_PrivateData *pd = vedata->stl->pd;
  const DRWContextState *draw_ctx = DRW_context_state_get();
  DRWShadingGroup *grp = NULL;

  View3DShading *shading = &draw_ctx->v3d->shading;

  pd->shdata.wire_step_param = pd->overlay.wireframe_threshold - 254.0f / 255.0f;

  bool is_wire_shmode = (shading->type == OB_WIRE);
  bool is_object_color = is_wire_shmode && (shading->wire_color_type == V3D_SHADING_OBJECT_COLOR);
  bool is_random_color = is_wire_shmode && (shading->wire_color_type == V3D_SHADING_RANDOM_COLOR);

  const bool use_select = (DRW_state_is_select() || DRW_state_is_depth());
  GPUShader *wires_sh = use_select ? OVERLAY_shader_wireframe_select() :
                                     OVERLAY_shader_wireframe();

  for (int xray = 0; xray < 2; xray++) {
    DRWState state = DRW_STATE_FIRST_VERTEX_CONVENTION;
    DRWPass *pass;
    uint stencil_mask;

    if (xray == 0) {
      state |= DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL |
               DRW_STATE_STENCIL_EQUAL;
      DRW_PASS_CREATE(psl->wireframe_ps, state | pd->clipping_state);
      pass = psl->wireframe_ps;
      stencil_mask = 0xFF;
    }
    else {
      state |= DRW_STATE_WRITE_DEPTH | DRW_STATE_WRITE_STENCIL | DRW_STATE_DEPTH_GREATER_EQUAL |
               DRW_STATE_STENCIL_NEQUAL;
      DRW_PASS_CREATE(psl->wireframe_xray_ps, state | pd->clipping_state);
      pass = psl->wireframe_xray_ps;
      stencil_mask = 0x00;
    }

    for (int use_coloring = 0; use_coloring < 2; use_coloring++) {
      pd->wires_grp[xray][use_coloring] = grp = DRW_shgroup_create(wires_sh, pass);
      DRW_shgroup_uniform_block_persistent(grp, "globalsBlock", G_draw.block_ubo);
      DRW_shgroup_uniform_float_copy(grp, "wireStepParam", pd->shdata.wire_step_param);
      DRW_shgroup_uniform_bool_copy(grp, "useColoring", use_coloring);
      DRW_shgroup_uniform_bool_copy(grp, "isTransform", (G.moving & G_TRANSFORM_OBJ) != 0);
      DRW_shgroup_uniform_bool_copy(grp, "isObjectColor", is_object_color);
      DRW_shgroup_uniform_bool_copy(grp, "isRandomColor", is_random_color);
      DRW_shgroup_stencil_mask(grp, stencil_mask);

      pd->wires_all_grp[xray][use_coloring] = grp = DRW_shgroup_create(wires_sh, pass);
      DRW_shgroup_uniform_float_copy(grp, "wireStepParam", 1.0f);
      DRW_shgroup_stencil_mask(grp, stencil_mask);
    }

    pd->wires_sculpt_grp[xray] = grp = DRW_shgroup_create(wires_sh, pass);
    DRW_shgroup_uniform_float_copy(grp, "wireStepParam", 10.0f);
    DRW_shgroup_uniform_bool_copy(grp, "useColoring", false);
    DRW_shgroup_stencil_mask(grp, stencil_mask);
  }
}

void OVERLAY_wireframe_cache_populate(OVERLAY_Data *vedata,
                                      Object *ob,
                                      OVERLAY_DupliData *dupli,
                                      bool init_dupli)
{
  OVERLAY_Data *data = vedata;
  OVERLAY_PrivateData *pd = data->stl->pd;
  const DRWContextState *draw_ctx = DRW_context_state_get();
  const bool all_wires = (ob->dtx & OB_DRAW_ALL_EDGES) != 0;
  const bool is_xray = (ob->dtx & OB_DRAWXRAY) != 0;
  const bool is_mesh = ob->type == OB_MESH;
  const bool use_wire = (pd->overlay.flag & V3D_OVERLAY_WIREFRAMES) || (ob->dtx & OB_DRAWWIRE) ||
                        (ob->dt == OB_WIRE);

  /* Fast path for duplis. */
  if (dupli && !init_dupli) {
    if (dupli->wire_shgrp && dupli->wire_geom) {
      if (dupli->base_flag == ob->base_flag) {
        DRW_shgroup_call(dupli->wire_shgrp, dupli->wire_geom, ob);
        return;
      }
    }
    else {
      /* Nothing to draw for this dupli. */
      return;
    }
  }

  const bool is_edit_mode = BKE_object_is_in_editmode(ob);
  bool has_edit_mesh_cage = false;
  if (is_mesh && is_edit_mode) {
    /* TODO: Should be its own function. */
    Mesh *me = (Mesh *)ob->data;
    BMEditMesh *embm = me->edit_mesh;
    if (embm) {
      has_edit_mesh_cage = embm->mesh_eval_cage && (embm->mesh_eval_cage != embm->mesh_eval_final);
    }
  }

  /* Don't do that in edit Mesh mode, unless there is a modifier preview. */
  if (use_wire && (!is_mesh || (!is_edit_mode || has_edit_mesh_cage))) {
    const bool use_sculpt_pbvh = BKE_sculptsession_use_pbvh_draw(ob, draw_ctx->v3d) &&
                                 !DRW_state_is_image_render();
    const bool use_coloring = (use_wire && !is_edit_mode && !use_sculpt_pbvh &&
                               !has_edit_mesh_cage);
    DRWShadingGroup *shgrp = NULL;
    struct GPUBatch *geom = DRW_cache_object_face_wireframe_get(ob);

    if (geom || use_sculpt_pbvh) {
      if (use_sculpt_pbvh) {
        shgrp = pd->wires_sculpt_grp[is_xray];
      }
      else if (all_wires) {
        shgrp = pd->wires_all_grp[is_xray][use_coloring];
      }
      else {
        shgrp = pd->wires_grp[is_xray][use_coloring];
      }

      if (use_sculpt_pbvh) {
        DRW_shgroup_call_sculpt(shgrp, ob, true, false, false);
      }
      else {
        DRW_shgroup_call(shgrp, geom, ob);
      }
    }

    if (dupli) {
      dupli->wire_shgrp = shgrp;
      dupli->wire_geom = geom;
    }
  }
  else if (is_mesh && (!is_edit_mode || has_edit_mesh_cage)) {
    OVERLAY_ExtraCallBuffers *cb = OVERLAY_extra_call_buffer_get(vedata, ob);
    Mesh *me = ob->data;
    float *color;
    DRW_object_wire_theme_get(ob, draw_ctx->view_layer, &color);

    /* Draw loose geometry. */
    if (me->totedge > 0 || has_edit_mesh_cage) {
      struct GPUBatch *geom = DRW_cache_mesh_loose_edges_get(ob);
      if (geom) {
        OVERLAY_extra_wire(cb, geom, ob->obmat, color);
      }
    }
    else if (me->totedge == 0) {
      struct GPUBatch *geom = DRW_cache_mesh_all_verts_get(ob);
      if (geom) {
        OVERLAY_extra_loose_points(cb, geom, ob->obmat, color);
      }
    }
  }
}

void OVERLAY_wireframe_draw(OVERLAY_Data *data)
{
  OVERLAY_PassList *psl = data->psl;
  OVERLAY_PrivateData *pd = data->stl->pd;

  DRW_view_set_active(pd->view_wires);
  DRW_draw_pass(psl->wireframe_ps);

  DRW_view_set_active(pd->view_default);
}

void OVERLAY_wireframe_in_front_draw(OVERLAY_Data *data)
{
  OVERLAY_PassList *psl = data->psl;
  OVERLAY_PrivateData *pd = data->stl->pd;

  DRW_view_set_active(pd->view_wires);

  /* First lower the depth where there is no xray object (and set the stencil to xray value). */
  DRW_draw_pass(psl->wireframe_xray_ps);

  /* Then draw the xray wire on top only where the stencil is set. */
  DRWState state_remove = DRW_STATE_DEPTH_GREATER_EQUAL | DRW_STATE_STENCIL_NEQUAL;
  DRWState state_add = DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL |
                       DRW_STATE_STENCIL_ALWAYS;
  DRW_pass_state_remove(psl->wireframe_xray_ps, state_remove);
  DRW_pass_state_add(psl->wireframe_xray_ps, state_add);
  DRW_draw_pass(psl->wireframe_xray_ps);

  DRW_view_set_active(pd->view_default);
}

/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "IO_ocean_lod_disabler.hh"

#include "BLI_listbase.h"

#include "BKE_scene.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "DNA_layer_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

namespace blender::io {

OceanCameraLODDisabler::OceanCameraLODDisabler(Depsgraph *depsgraph) : depsgraph_(depsgraph) {}

OceanCameraLODDisabler::~OceanCameraLODDisabler()
{
  for (OceanModifierData *omd : disabled_modifiers_) {
    omd->flag |= MOD_OCEAN_USE_CAMERA_LOD;
  }

  for (Object *object : modified_objects_) {
    DEG_id_tag_update(&object->id, ID_RECALC_GEOMETRY);
  }
}

void OceanCameraLODDisabler::disable_modifiers()
{
  disable_modifiers([](const Object & /*object*/) { return true; });
}

void OceanCameraLODDisabler::disable_modifiers(FunctionRef<bool(const Object &object)> object_filter)
{
  DEGObjectIterSettings deg_iter_settings{};
  deg_iter_settings.depsgraph = depsgraph_;
  deg_iter_settings.flags = DEG_ITER_OBJECT_FLAG_LINKED_DIRECTLY |
                            DEG_ITER_OBJECT_FLAG_LINKED_VIA_SET |
                            DEG_ITER_OBJECT_FLAG_VISIBLE | DEG_ITER_OBJECT_FLAG_DUPLI;
  DEG_OBJECT_ITER_BEGIN (&deg_iter_settings, object_eval) {
    if (!object_filter(*object_eval)) {
      continue;
    }

    Object *object = DEG_get_original(object_eval);
    if (object == nullptr) {
      object = object_eval;
    }

    bool object_changed = false;
    for (ModifierData *md = static_cast<ModifierData *>(object->modifiers.first); md != nullptr;
         md = md->next)
    {
      if (md->type != eModifierType_Ocean) {
        continue;
      }
      OceanModifierData *omd = reinterpret_cast<OceanModifierData *>(md);
      if ((omd->flag & MOD_OCEAN_USE_CAMERA_LOD) == 0) {
        continue;
      }
      disable_modifier(omd);
      object_changed = true;
    }
    if (object_changed) {
      modified_objects_.append(object);
      DEG_id_tag_update(&object->id, ID_RECALC_GEOMETRY);
    }
  }
  DEG_OBJECT_ITER_END;

  if (has_disabled_modifiers()) {
    BKE_scene_graph_update_tagged(depsgraph_, DEG_get_bmain(depsgraph_));
  }
}

bool OceanCameraLODDisabler::has_disabled_modifiers() const
{
  return !disabled_modifiers_.is_empty();
}

void OceanCameraLODDisabler::disable_modifier(OceanModifierData *omd)
{
  omd->flag &= ~MOD_OCEAN_USE_CAMERA_LOD;
  disabled_modifiers_.append(omd);
}

}  // namespace blender::io

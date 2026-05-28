/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "BLI_vector.hh"

namespace blender {

struct Depsgraph;
struct Object;
struct OceanModifierData;

namespace io {

/**
 * Temporarily disable view-dependent Ocean Camera LOD for stable mesh export.
 *
 * The destructor restores all disabled modifiers.
 */
class OceanCameraLODDisabler final {
 private:
  Depsgraph *depsgraph_;
  Vector<OceanModifierData *> disabled_modifiers_;
  Vector<Object *> modified_objects_;

 public:
  explicit OceanCameraLODDisabler(Depsgraph *depsgraph);
  ~OceanCameraLODDisabler();

  void disable_modifiers();
  bool has_disabled_modifiers() const;

  OceanCameraLODDisabler(const OceanCameraLODDisabler &) = delete;
  OceanCameraLODDisabler &operator=(const OceanCameraLODDisabler &) = delete;

 private:
  void disable_modifier(OceanModifierData *omd);
};

}  // namespace io
}  // namespace blender

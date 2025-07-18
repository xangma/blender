/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "GEO_mesh_copy_selection.hh"
#include "GEO_randomize.hh"

#include "BKE_curves.hh"
#include "BKE_instances.hh"
#include "BKE_pointcloud.hh"

#include "NOD_rna_define.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_enum_types.hh"

#include "BLI_array_utils.hh"

namespace blender::nodes::node_geo_split_to_instances_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry")
      .supported_type({GeometryComponent::Type::Mesh,
                       GeometryComponent::Type::PointCloud,
                       GeometryComponent::Type::Curve,
                       GeometryComponent::Type::Instance})
      .description("Geometry to split into instances");
  b.add_input<decl::Bool>("Selection").default_value(true).field_on_all().hide_value();
  b.add_input<decl::Int>("Group ID").field_on_all().hide_value();
  b.add_output<decl::Geometry>("Instances")
      .propagate_all()
      .description("All geometry groups as separate instances");
  b.add_output<decl::Int>("Group ID")
      .field_on_all()
      .description("The group ID of each group instance");
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);
  layout->prop(ptr, "domain", UI_ITEM_NONE, "", ICON_NONE);
}

static void ensure_group_geometries(Map<int, std::unique_ptr<GeometrySet>> &geometry_by_group_id,
                                    const Span<int> group_ids)
{
  for (const int group_id : group_ids) {
    geometry_by_group_id.lookup_or_add_cb(group_id,
                                          []() { return std::make_unique<GeometrySet>(); });
  }
}

struct SplitGroups {
  std::optional<bke::GeometryFieldContext> field_context;
  std::optional<FieldEvaluator> field_evaluator;

  VectorSet<int> group_ids;

  IndexMaskMemory memory;
  Vector<IndexMask> group_masks;
};

/**
 * \return True, if the component is already fully handled and does not need further processing.
 */
[[nodiscard]] static bool do_common_split(
    const GeometryComponent &src_component,
    const AttrDomain domain,
    const Field<bool> &selection_field,
    const Field<int> &group_id_field,
    Map<int, std::unique_ptr<GeometrySet>> &geometry_by_group_id,
    SplitGroups &r_groups)
{
  const int domain_size = src_component.attribute_domain_size(domain);

  r_groups.field_context.emplace(src_component, domain);
  FieldEvaluator &field_evaluator = r_groups.field_evaluator.emplace(*r_groups.field_context,
                                                                     domain_size);
  field_evaluator.set_selection(selection_field);
  field_evaluator.add(group_id_field);
  field_evaluator.evaluate();

  const IndexMask selection = field_evaluator.get_evaluated_selection_as_mask();
  if (selection.is_empty()) {
    return true;
  }

  r_groups.group_masks = IndexMask::from_group_ids(
      selection, field_evaluator.get_evaluated<int>(0), r_groups.memory, r_groups.group_ids);

  ensure_group_geometries(geometry_by_group_id, r_groups.group_ids);
  return false;
}

static void split_mesh_groups(const MeshComponent &component,
                              const AttrDomain domain,
                              const Field<bool> &selection_field,
                              const Field<int> &group_id_field,
                              const AttributeFilter &attribute_filter,
                              Map<int, std::unique_ptr<GeometrySet>> &geometry_by_group_id)
{
  SplitGroups split_groups;
  if (do_common_split(
          component, domain, selection_field, group_id_field, geometry_by_group_id, split_groups))
  {
    return;
  }
  const Mesh &src_mesh = *component.get();
  const int domain_size = component.attribute_domain_size(domain);

  threading::EnumerableThreadSpecific<Array<bool>> group_selection_per_thread{
      [&]() { return Array<bool>(domain_size, false); }};

  threading::parallel_for(split_groups.group_masks.index_range(), 16, [&](const IndexRange range) {
    /* Need task isolation because of the thread local variable. */
    threading::isolate_task([&]() {
      MutableSpan<bool> group_selection = group_selection_per_thread.local();
      const VArray<bool> group_selection_varray = VArray<bool>::from_span(group_selection);
      for (const int group_index : range) {
        const IndexMask &mask = split_groups.group_masks[group_index];
        index_mask::masked_fill(group_selection, true, mask);
        const int group_id = split_groups.group_ids[group_index];

        /* Using #mesh_copy_selection here is not ideal, because it can lead to O(n^2) behavior
         * when there are many groups. */
        std::optional<Mesh *> group_mesh_opt = geometry::mesh_copy_selection(
            src_mesh, group_selection_varray, domain, attribute_filter);
        GeometrySet &group_geometry = *geometry_by_group_id.lookup(group_id);
        if (group_mesh_opt.has_value()) {
          if (Mesh *group_mesh = *group_mesh_opt) {
            group_geometry.replace_mesh(group_mesh);
          }
          else {
            group_geometry.replace_mesh(nullptr);
          }
        }
        else {
          group_geometry.add(component);
        }

        index_mask::masked_fill(group_selection, false, mask);
      }
    });
  });
}

static void split_pointcloud_groups(const PointCloudComponent &component,
                                    const Field<bool> &selection_field,
                                    const Field<int> &group_id_field,
                                    const AttributeFilter &attribute_filter,
                                    Map<int, std::unique_ptr<GeometrySet>> &geometry_by_group_id)
{
  SplitGroups split_groups;
  if (do_common_split(component,
                      AttrDomain::Point,
                      selection_field,
                      group_id_field,
                      geometry_by_group_id,
                      split_groups))
  {
    return;
  }
  const PointCloud &src_pointcloud = *component.get();
  threading::parallel_for(split_groups.group_ids.index_range(), 16, [&](const IndexRange range) {
    for (const int group_index : range) {
      const IndexMask &mask = split_groups.group_masks[group_index];
      const int group_id = split_groups.group_ids[group_index];

      PointCloud *group_pointcloud = BKE_pointcloud_new_nomain(mask.size());

      const AttributeAccessor src_attributes = src_pointcloud.attributes();
      MutableAttributeAccessor dst_attributes = group_pointcloud->attributes_for_write();
      bke::gather_attributes(src_attributes,
                             AttrDomain::Point,
                             AttrDomain::Point,
                             attribute_filter,
                             mask,
                             dst_attributes);

      GeometrySet &group_geometry = *geometry_by_group_id.lookup(group_id);
      group_geometry.replace_pointcloud(group_pointcloud);
    }
  });
}

static void split_curve_groups(const bke::CurveComponent &component,
                               const AttrDomain domain,
                               const Field<bool> &selection_field,
                               const Field<int> &group_id_field,
                               const AttributeFilter &attribute_filter,
                               Map<int, std::unique_ptr<GeometrySet>> &geometry_by_group_id)
{
  SplitGroups split_groups;
  if (do_common_split(
          component, domain, selection_field, group_id_field, geometry_by_group_id, split_groups))
  {
    return;
  }
  const bke::CurvesGeometry &src_curves = component.get()->geometry.wrap();
  threading::parallel_for(split_groups.group_ids.index_range(), 16, [&](const IndexRange range) {
    for (const int group_index : range) {
      const IndexMask &mask = split_groups.group_masks[group_index];
      const int group_id = split_groups.group_ids[group_index];

      bke::CurvesGeometry group_curves;
      if (domain == AttrDomain::Point) {
        group_curves = bke::curves_copy_point_selection(src_curves, mask, attribute_filter);
      }
      else {
        group_curves = bke::curves_copy_curve_selection(src_curves, mask, attribute_filter);
      }
      Curves *group_curves_id = bke::curves_new_nomain(std::move(group_curves));
      GeometrySet &group_geometry = *geometry_by_group_id.lookup(group_id);
      group_geometry.replace_curves(group_curves_id);
    }
  });
}

static void split_instance_groups(const InstancesComponent &component,
                                  const Field<bool> &selection_field,
                                  const Field<int> &group_id_field,
                                  const AttributeFilter &attribute_filter,
                                  Map<int, std::unique_ptr<GeometrySet>> &geometry_by_group_id)
{
  SplitGroups split_groups;
  if (do_common_split(component,
                      AttrDomain::Instance,
                      selection_field,
                      group_id_field,
                      geometry_by_group_id,
                      split_groups))
  {
    return;
  }
  const bke::Instances &src_instances = *component.get();
  threading::parallel_for(split_groups.group_ids.index_range(), 16, [&](const IndexRange range) {
    for (const int group_index : range) {
      const IndexMask &mask = split_groups.group_masks[group_index];
      const int group_id = split_groups.group_ids[group_index];

      bke::Instances *group_instances = new bke::Instances();
      group_instances->resize(mask.size());

      for (const bke::InstanceReference &reference : src_instances.references()) {
        group_instances->add_reference(reference);
      }

      bke::gather_attributes(src_instances.attributes(),
                             AttrDomain::Instance,
                             AttrDomain::Instance,
                             attribute_filter,
                             mask,
                             group_instances->attributes_for_write());
      group_instances->remove_unused_references();

      GeometrySet &group_geometry = *geometry_by_group_id.lookup(group_id);
      group_geometry.replace_instances(group_instances);
    }
  });
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const bNode &node = params.node();
  const AttrDomain domain = AttrDomain(node.custom1);

  GeometrySet src_geometry = params.extract_input<GeometrySet>("Geometry");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  const Field<int> group_id_field = params.extract_input<Field<int>>("Group ID");

  const NodeAttributeFilter &attribute_filter = params.get_attribute_filter("Instances");

  Map<int, std::unique_ptr<GeometrySet>> geometry_by_group_id;

  if (src_geometry.has_mesh() &&
      ELEM(domain, AttrDomain::Point, AttrDomain::Edge, AttrDomain::Face))
  {
    const auto &component = *src_geometry.get_component<MeshComponent>();
    split_mesh_groups(component,
                      domain,
                      selection_field,
                      group_id_field,
                      attribute_filter,
                      geometry_by_group_id);
  }
  if (src_geometry.has_pointcloud() && domain == AttrDomain::Point) {
    const auto &component = *src_geometry.get_component<PointCloudComponent>();
    split_pointcloud_groups(
        component, selection_field, group_id_field, attribute_filter, geometry_by_group_id);
  }
  if (src_geometry.has_curves() && ELEM(domain, AttrDomain::Point, AttrDomain::Curve)) {
    const auto &component = *src_geometry.get_component<bke::CurveComponent>();
    split_curve_groups(component,
                       domain,
                       selection_field,
                       group_id_field,
                       attribute_filter,
                       geometry_by_group_id);
  }
  if (src_geometry.has_instances() && domain == AttrDomain::Instance) {
    const auto &component = *src_geometry.get_component<bke::InstancesComponent>();
    split_instance_groups(
        component, selection_field, group_id_field, attribute_filter, geometry_by_group_id);
  }

  bke::Instances *dst_instances = new bke::Instances();
  GeometrySet dst_geometry = GeometrySet::from_instances(dst_instances);
  const int total_groups_num = geometry_by_group_id.size();
  dst_instances->resize(total_groups_num);

  std::optional<std::string> dst_group_id_attribute_id =
      params.get_output_anonymous_attribute_id_if_needed("Group ID");
  if (dst_group_id_attribute_id) {
    SpanAttributeWriter<int> dst_group_id =
        dst_instances->attributes_for_write().lookup_or_add_for_write_span<int>(
            *dst_group_id_attribute_id, AttrDomain::Instance);
    std::copy(geometry_by_group_id.keys().begin(),
              geometry_by_group_id.keys().end(),
              dst_group_id.span.begin());
    dst_group_id.finish();
  }

  dst_instances->transforms_for_write().fill(float4x4::identity());
  array_utils::fill_index_range(dst_instances->reference_handles_for_write());

  for (auto item : geometry_by_group_id.items()) {
    std::unique_ptr<GeometrySet> &group_geometry = item.value;
    dst_instances->add_reference(std::move(group_geometry));
  }

  dst_geometry.name = src_geometry.name;

  geometry::debug_randomize_instance_order(dst_instances);

  params.set_output("Instances", std::move(dst_geometry));
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(srna,
                    "domain",
                    "Domain",
                    "Attribute domain for the Selection and Group ID inputs",
                    rna_enum_attribute_domain_without_corner_items,
                    NOD_inline_enum_accessors(custom1),
                    int(AttrDomain::Point));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeSplitToInstances", GEO_NODE_SPLIT_TO_INSTANCES);
  ntype.ui_name = "Split to Instances";
  ntype.ui_description = "Create separate geometries containing the elements from the same group";
  ntype.enum_name_legacy = "Split to Instances";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_split_to_instances_cc

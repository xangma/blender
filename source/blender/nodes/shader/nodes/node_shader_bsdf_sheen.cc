/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_shader_bsdf_sheen_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>("Color").default_value({0.8f, 0.8f, 0.8f, 1.0f});
  b.add_input<decl::Float>("Roughness")
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR);
  b.add_input<decl::Vector>("Normal").hide_value();
  b.add_input<decl::Float>("Weight").available(false);
  b.add_output<decl::Shader>("BSDF");
}

static void node_shader_buts_sheen(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "distribution", UI_ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);
}

static void node_shader_init_sheen(bNodeTree * /*ntree*/, bNode *node)
{
  node->custom1 = SHD_SHEEN_MICROFIBER;
}

static int node_shader_gpu_bsdf_sheen(GPUMaterial *mat,
                                      bNode *node,
                                      bNodeExecData * /*execdata*/,
                                      GPUNodeStack *in,
                                      GPUNodeStack *out)
{
  if (!in[2].link) {
    GPU_link(mat, "world_normals_get", &in[2].link);
  }

  GPU_material_flag_set(mat, GPU_MATFLAG_DIFFUSE);

  return GPU_stack_link(mat, node, "node_bsdf_sheen", in, out);
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  if (to_type_ != NodeItem::Type::BSDF) {
    return empty();
  }

  NodeItem color = get_input_value("Color", NodeItem::Type::Color3);
  NodeItem roughness = get_input_value("Roughness", NodeItem::Type::Float);
  NodeItem normal = get_input_link("Normal", NodeItem::Type::Vector3);
#  if !(MATERIALX_MAJOR_VERSION <= 1 && MATERIALX_MINOR_VERSION <= 38)
  NodeItem mode = node_->custom1 == SHD_SHEEN_MICROFIBER ? val(std::string("zeltner")) :
                                                           val(std::string("conty_kulla"));
#  endif

  return create_node("sheen_bsdf",
                     NodeItem::Type::BSDF,
                     {{"color", color},
                      {"roughness", roughness},
                      {"normal", normal}
#  if !(MATERIALX_MAJOR_VERSION <= 1 && MATERIALX_MINOR_VERSION <= 38)
                      ,
                      {"mode", mode}});
#  else
                     });
#  endif
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace blender::nodes::node_shader_bsdf_sheen_cc

/* node type definition */
void register_node_type_sh_bsdf_sheen()
{
  namespace file_ns = blender::nodes::node_shader_bsdf_sheen_cc;

  static blender::bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeBsdfSheen", SH_NODE_BSDF_SHEEN);
  ntype.ui_name = "Sheen BSDF";
  ntype.ui_description =
      "Reflection for materials such as cloth.\nTypically mixed with other shaders (such as a "
      "Diffuse Shader) and is not particularly useful on its own";
  ntype.enum_name_legacy = "BSDF_SHEEN";
  ntype.nclass = NODE_CLASS_SHADER;
  ntype.add_ui_poll = object_cycles_shader_nodes_poll;
  ntype.declare = file_ns::node_declare;
  ntype.initfunc = file_ns::node_shader_init_sheen;
  ntype.gpu_fn = file_ns::node_shader_gpu_bsdf_sheen;
  ntype.draw_buttons = file_ns::node_shader_buts_sheen;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  blender::bke::node_register_type(ntype);
}

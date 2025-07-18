/* SPDX-FileCopyrightText: 2014 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cstdlib>

#include "CLG_log.h"

#include "DNA_anim_types.h"

#include "BLI_function_ref.hh"
#include "BLI_ghash.h"
#include "BLI_linklist_stack.h"
#include "BLI_listbase.h"
#include "BLI_set.hh"

#include "BKE_anim_data.hh"
#include "BKE_idprop.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"

static CLG_LogRef LOG = {"lib.query"};

/* status */
enum {
  IDWALK_STOP = 1 << 0,
};

struct LibraryForeachIDData {
  Main *bmain;
  /**
   * 'Real' ID, the one that might be in `bmain`, only differs from self_id when the later is a
   * private one.
   */
  ID *owner_id;
  /**
   * ID from which the current ID pointer is being processed. It may be an embedded ID like master
   * collection or root node tree.
   */
  ID *self_id;

  /** Flags controlling the behavior of the 'foreach id' looping code. */
  LibraryForeachIDFlag flag;
  /** Generic flags to be passed to all callback calls for current processed data. */
  LibraryForeachIDCallbackFlag cb_flag;
  /** Callback flags that are forbidden for all callback calls for current processed data. */
  LibraryForeachIDCallbackFlag cb_flag_clear;

  /**
   * Function to call for every ID pointers of current processed data, and its opaque user data
   * pointer.
   */
  blender::FunctionRef<LibraryIDLinkCallback> callback;
  void *user_data;
  /**
   * Store the returned value from the callback, to decide how to continue the processing of ID
   * pointers for current data.
   */
  int status;

  /* To handle recursion. */
  GSet *ids_handled; /* All IDs that are either already done, or still in ids_todo stack. */
  BLI_LINKSTACK_DECLARE(ids_todo, ID *);
};

bool BKE_lib_query_foreachid_iter_stop(const LibraryForeachIDData *data)
{
  return (data->status & IDWALK_STOP) != 0;
}

void BKE_lib_query_foreachid_process(LibraryForeachIDData *data,
                                     ID **id_pp,
                                     LibraryForeachIDCallbackFlag cb_flag)
{
  if (BKE_lib_query_foreachid_iter_stop(data)) {
    return;
  }

  const LibraryForeachIDFlag flag = data->flag;
  ID *old_id = *id_pp;

  /* Update the callback flags with the ones defined (or forbidden) in `data` by the generic
   * caller code. */
  cb_flag = LibraryForeachIDCallbackFlag((cb_flag | data->cb_flag) & ~data->cb_flag_clear);

  /* Update the callback flags with some extra information regarding overrides: all "loop-back",
   * "internal", "embedded" etc. ID pointers are never overridable. */
  if (cb_flag & (IDWALK_CB_INTERNAL | IDWALK_CB_LOOPBACK | IDWALK_CB_OVERRIDE_LIBRARY_REFERENCE)) {
    cb_flag |= IDWALK_CB_OVERRIDE_LIBRARY_NOT_OVERRIDABLE;
  }

  LibraryIDLinkCallbackData callback_data{};
  callback_data.user_data = data->user_data;
  callback_data.bmain = data->bmain;
  callback_data.owner_id = data->owner_id;
  callback_data.self_id = data->self_id;
  callback_data.id_pointer = id_pp;
  callback_data.cb_flag = cb_flag;
  const int callback_return = data->callback(&callback_data);

  if (flag & IDWALK_READONLY) {
    BLI_assert(*(id_pp) == old_id);
  }
  else {
    BLI_assert_msg((callback_return & (IDWALK_RET_STOP_ITER | IDWALK_RET_STOP_RECURSION)) == 0,
                   "Iteration over ID usages should not be interrupted by the callback in "
                   "non-readonly cases");
  }

  if (old_id && (flag & IDWALK_RECURSE)) {
    if (BLI_gset_add((data)->ids_handled, old_id)) {
      if (!(callback_return & IDWALK_RET_STOP_RECURSION)) {
        BLI_LINKSTACK_PUSH(data->ids_todo, old_id);
      }
    }
  }
  if (callback_return & IDWALK_RET_STOP_ITER) {
    data->status |= IDWALK_STOP;
  }
}

LibraryForeachIDFlag BKE_lib_query_foreachid_process_flags_get(const LibraryForeachIDData *data)
{
  return data->flag;
}

Main *BKE_lib_query_foreachid_process_main_get(const LibraryForeachIDData *data)
{
  return data->bmain;
}

int BKE_lib_query_foreachid_process_callback_flag_override(
    LibraryForeachIDData *data, const LibraryForeachIDCallbackFlag cb_flag, const bool do_replace)
{
  const LibraryForeachIDCallbackFlag cb_flag_backup = data->cb_flag;
  if (do_replace) {
    data->cb_flag = cb_flag;
  }
  else {
    data->cb_flag |= cb_flag;
  }
  return cb_flag_backup;
}

static bool library_foreach_ID_link(Main *bmain,
                                    ID *owner_id,
                                    ID *id,
                                    blender::FunctionRef<LibraryIDLinkCallback> callback,
                                    void *user_data,
                                    LibraryForeachIDFlag flag,
                                    LibraryForeachIDData *inherit_data);

void BKE_lib_query_idpropertiesForeachIDLink_callback(IDProperty *id_prop, void *user_data)
{
  BLI_assert(id_prop->type == IDP_ID);

  LibraryForeachIDData *data = (LibraryForeachIDData *)user_data;
  const LibraryForeachIDCallbackFlag cb_flag = IDWALK_CB_USER |
                                               ((id_prop->flag & IDP_FLAG_OVERRIDABLE_LIBRARY) ?
                                                    IDWALK_CB_NOP :
                                                    IDWALK_CB_OVERRIDE_LIBRARY_NOT_OVERRIDABLE);
  BKE_LIB_FOREACHID_PROCESS_ID(data, id_prop->data.pointer, cb_flag);
}

void BKE_library_foreach_ID_embedded(LibraryForeachIDData *data, ID **id_pp)
{
  /* Needed e.g. for callbacks handling relationships. This call should be absolutely read-only. */
  ID *id = *id_pp;
  const LibraryForeachIDFlag flag = data->flag;

  BKE_lib_query_foreachid_process(data, id_pp, IDWALK_CB_EMBEDDED);
  if (BKE_lib_query_foreachid_iter_stop(data)) {
    return;
  }
  BLI_assert(id == *id_pp);

  if (id == nullptr) {
    return;
  }

  if (flag & IDWALK_IGNORE_EMBEDDED_ID) {
    /* Do Nothing. */
  }
  else if (flag & IDWALK_RECURSE) {
    /* Defer handling into main loop, recursively calling BKE_library_foreach_ID_link in
     * IDWALK_RECURSE case is troublesome, see #49553. */
    if (BLI_gset_add(data->ids_handled, id)) {
      BLI_LINKSTACK_PUSH(data->ids_todo, id);
    }
  }
  else {
    if (!library_foreach_ID_link(
            data->bmain, data->owner_id, id, data->callback, data->user_data, data->flag, data))
    {
      data->status |= IDWALK_STOP;
      return;
    }
  }
}

static void library_foreach_ID_data_cleanup(LibraryForeachIDData *data)
{
  if (data->ids_handled != nullptr) {
    BLI_gset_free(data->ids_handled, nullptr);
    BLI_LINKSTACK_FREE(data->ids_todo);
  }
}

/** \return false in case iteration over ID pointers must be stopped, true otherwise. */
static bool library_foreach_ID_link(Main *bmain,
                                    ID *owner_id,
                                    ID *id,
                                    blender::FunctionRef<LibraryIDLinkCallback> callback,
                                    void *user_data,
                                    LibraryForeachIDFlag flag,
                                    LibraryForeachIDData *inherit_data)
{
  LibraryForeachIDData data{};
  data.bmain = bmain;

  BLI_assert(inherit_data == nullptr || data.bmain == inherit_data->bmain);
  /* `IDWALK_NO_ORIG_POINTERS_ACCESS` is mutually exclusive with `IDWALK_RECURSE`. */
  BLI_assert((flag & (IDWALK_NO_ORIG_POINTERS_ACCESS | IDWALK_RECURSE)) !=
             (IDWALK_NO_ORIG_POINTERS_ACCESS | IDWALK_RECURSE));

  if (flag & IDWALK_NO_ORIG_POINTERS_ACCESS) {
    flag |= IDWALK_IGNORE_MISSING_OWNER_ID;
  }

  if (flag & IDWALK_RECURSE) {
    /* For now, recursion implies read-only, and no internal pointers. */
    flag |= IDWALK_READONLY;
    flag &= ~IDWALK_DO_INTERNAL_RUNTIME_POINTERS;

    /* NOTE: This function itself should never be called recursively when IDWALK_RECURSE is set,
     * see also comments in #BKE_library_foreach_ID_embedded.
     * This is why we can always create this data here, and do not need to try and re-use it from
     * `inherit_data`. */
    data.ids_handled = BLI_gset_new(BLI_ghashutil_ptrhash, BLI_ghashutil_ptrcmp, __func__);
    BLI_LINKSTACK_INIT(data.ids_todo);

    BLI_gset_add(data.ids_handled, id);
  }
  else {
    data.ids_handled = nullptr;
  }
  data.flag = flag;
  data.status = 0;
  data.callback = callback;
  data.user_data = user_data;

#define CALLBACK_INVOKE_ID(check_id, cb_flag) \
  { \
    CHECK_TYPE_ANY((check_id), ID *, void *); \
    BKE_lib_query_foreachid_process(&data, (ID **)&(check_id), (cb_flag)); \
    if (BKE_lib_query_foreachid_iter_stop(&data)) { \
      library_foreach_ID_data_cleanup(&data); \
      return false; \
    } \
  } \
  ((void)0)

#define CALLBACK_INVOKE(check_id_super, cb_flag) \
  { \
    CHECK_TYPE(&((check_id_super)->id), ID *); \
    BKE_lib_query_foreachid_process(&data, (ID **)&(check_id_super), (cb_flag)); \
    if (BKE_lib_query_foreachid_iter_stop(&data)) { \
      library_foreach_ID_data_cleanup(&data); \
      return false; \
    } \
  } \
  ((void)0)

  for (; id != nullptr; id = (flag & IDWALK_RECURSE) ? BLI_LINKSTACK_POP(data.ids_todo) : nullptr,
                        owner_id = nullptr)
  {
    data.self_id = id;
    /* owner ID is same as self ID, except for embedded ID case. */
    if (id->flag & ID_FLAG_EMBEDDED_DATA) {
      if (flag & IDWALK_IGNORE_MISSING_OWNER_ID) {
        data.owner_id = owner_id ? owner_id : id;
      }
      else {
        /* NOTE: Unfortunately it is not possible to ensure validity of the set owner_id pointer
         * here. `foreach_id` is used a lot by code remapping pointers, and in such cases the
         * current owner ID of the processed embedded ID is indeed invalid - and the given one is
         * to be assumed valid for the purpose of the current process.
         *
         * In other words, it is the responsibility of the code calling this `foreach_id` process
         * to ensure that the given owner ID is valid for its own purpose, or that it is not used.
         */
        // BLI_assert(owner_id == nullptr || BKE_id_owner_get(id) == owner_id);
        if (!owner_id) {
          owner_id = BKE_id_owner_get(id, false);
        }
        data.owner_id = owner_id;
      }
    }
    else {
      BLI_assert(ELEM(owner_id, nullptr, id));
      data.owner_id = id;
    }

    /* inherit_data is non-nullptr when this function is called for some sub-data ID
     * (like root node-tree of a material).
     * In that case, we do not want to generate those 'generic flags' from our current sub-data ID
     * (the node tree), but re-use those generated for the 'owner' ID (the material). */
    if (inherit_data == nullptr) {
      data.cb_flag = ID_IS_LINKED(id) ? IDWALK_CB_INDIRECT_USAGE : IDWALK_CB_NOP;
      /* When an ID is defined as not reference-counting its ID usages, it should never do it. */
      data.cb_flag_clear = (id->tag & ID_TAG_NO_USER_REFCOUNT) ?
                               IDWALK_CB_USER | IDWALK_CB_USER_ONE :
                               IDWALK_CB_NOP;
    }
    else {
      data.cb_flag = inherit_data->cb_flag;
      data.cb_flag_clear = inherit_data->cb_flag_clear;
    }

    bool use_bmain_relations = bmain != nullptr && bmain->relations != nullptr &&
                               (flag & IDWALK_READONLY);
    /* Including UI-related ID pointers should match with the relevant setting in Main relations
     * cache. */
    if (use_bmain_relations && (((bmain->relations->flag & MAINIDRELATIONS_INCLUDE_UI) == 0) !=
                                ((data.flag & IDWALK_INCLUDE_UI) == 0)))
    {
      use_bmain_relations = false;
    }
    /* No special 'internal' handling of ID pointers is covered by Main relations cache. */
    if (use_bmain_relations &&
        (flag & (IDWALK_DO_INTERNAL_RUNTIME_POINTERS | IDWALK_DO_LIBRARY_POINTER |
                 IDWALK_DO_DEPRECATED_POINTERS)))
    {
      use_bmain_relations = false;
    }
    if (use_bmain_relations) {
      /* Note that this is minor optimization, even in worst cases (like id being an object with
       * lots of drivers and constraints and modifiers, or material etc. with huge node tree),
       * but we might as well use it (Main->relations is always assumed valid,
       * it's responsibility of code creating it to free it,
       * especially if/when it starts modifying Main database). */
      MainIDRelationsEntry *entry = static_cast<MainIDRelationsEntry *>(
          BLI_ghash_lookup(bmain->relations->relations_from_pointers, id));
      for (MainIDRelationsEntryItem *to_id_entry = entry->to_ids; to_id_entry != nullptr;
           to_id_entry = to_id_entry->next)
      {
        BKE_lib_query_foreachid_process(
            &data, to_id_entry->id_pointer.to, to_id_entry->usage_flag);
        if (BKE_lib_query_foreachid_iter_stop(&data)) {
          library_foreach_ID_data_cleanup(&data);
          return false;
        }
      }
      continue;
    }

    if (flag & IDWALK_DO_LIBRARY_POINTER) {
      CALLBACK_INVOKE(id->lib, IDWALK_CB_NEVER_SELF);
    }

    if (flag & IDWALK_DO_INTERNAL_RUNTIME_POINTERS) {
      CALLBACK_INVOKE_ID(id->newid, IDWALK_CB_INTERNAL);
      CALLBACK_INVOKE_ID(id->orig_id, IDWALK_CB_INTERNAL);
    }

    if (id->override_library != nullptr) {
      CALLBACK_INVOKE_ID(id->override_library->reference,
                         IDWALK_CB_USER | IDWALK_CB_OVERRIDE_LIBRARY_REFERENCE);

      CALLBACK_INVOKE_ID(id->override_library->hierarchy_root, IDWALK_CB_LOOPBACK);
      LISTBASE_FOREACH (IDOverrideLibraryProperty *, op, &id->override_library->properties) {
        LISTBASE_FOREACH (IDOverrideLibraryPropertyOperation *, opop, &op->operations) {
          CALLBACK_INVOKE_ID(opop->subitem_reference_id,
                             IDWALK_CB_DIRECT_WEAK_LINK | IDWALK_CB_OVERRIDE_LIBRARY_REFERENCE);
          CALLBACK_INVOKE_ID(opop->subitem_local_id,
                             IDWALK_CB_DIRECT_WEAK_LINK | IDWALK_CB_OVERRIDE_LIBRARY_REFERENCE);
        }
      }
    }

    IDP_foreach_property(id->properties, IDP_TYPE_FILTER_ID, [&](IDProperty *prop) {
      BKE_lib_query_idpropertiesForeachIDLink_callback(prop, &data);
    });
    if (BKE_lib_query_foreachid_iter_stop(&data)) {
      library_foreach_ID_data_cleanup(&data);
      return false;
    }

    IDP_foreach_property(id->system_properties, IDP_TYPE_FILTER_ID, [&](IDProperty *prop) {
      BKE_lib_query_idpropertiesForeachIDLink_callback(prop, &data);
    });
    if (BKE_lib_query_foreachid_iter_stop(&data)) {
      library_foreach_ID_data_cleanup(&data);
      return false;
    }

    AnimData *adt = BKE_animdata_from_id(id);
    if (adt) {
      BKE_animdata_foreach_id(adt, &data);
      if (BKE_lib_query_foreachid_iter_stop(&data)) {
        library_foreach_ID_data_cleanup(&data);
        return false;
      }
    }

    const IDTypeInfo *id_type = BKE_idtype_get_info_from_id(id);
    if (id_type->foreach_id != nullptr) {
      id_type->foreach_id(id, &data);

      if (BKE_lib_query_foreachid_iter_stop(&data)) {
        library_foreach_ID_data_cleanup(&data);
        return false;
      }
    }
  }

  library_foreach_ID_data_cleanup(&data);
  return true;

#undef CALLBACK_INVOKE_ID
#undef CALLBACK_INVOKE
}

void BKE_library_foreach_ID_link(Main *bmain,
                                 ID *id,
                                 blender::FunctionRef<LibraryIDLinkCallback> callback,
                                 void *user_data,
                                 const LibraryForeachIDFlag flag)
{
  library_foreach_ID_link(bmain, nullptr, id, callback, user_data, flag, nullptr);
}

void BKE_library_update_ID_link_user(ID *id_dst, ID *id_src, const int cb_flag)
{
  if (cb_flag & IDWALK_CB_USER) {
    id_us_min(id_src);
    id_us_plus(id_dst);
  }
  else if (cb_flag & IDWALK_CB_USER_ONE) {
    id_us_ensure_real(id_dst);
  }
}

void BKE_library_foreach_subdata_id(
    Main *bmain,
    ID *owner_id,
    ID *self_id,
    blender::FunctionRef<void(LibraryForeachIDData *data)> subdata_foreach_id,
    blender::FunctionRef<LibraryIDLinkCallback> callback,
    void *user_data,
    const LibraryForeachIDFlag flag)
{
  BLI_assert((flag & (IDWALK_RECURSE | IDWALK_DO_INTERNAL_RUNTIME_POINTERS |
                      IDWALK_DO_LIBRARY_POINTER | IDWALK_INCLUDE_UI)) == 0);

  LibraryForeachIDData data{};
  data.bmain = bmain;
  data.owner_id = owner_id;
  data.self_id = self_id;
  data.ids_handled = nullptr;
  data.flag = flag;
  data.status = 0;
  data.callback = callback;
  data.user_data = user_data;

  subdata_foreach_id(&data);
}

uint64_t BKE_library_id_can_use_filter_id(const ID *owner_id,
                                          const bool include_ui,
                                          const IDTypeInfo *owner_id_type)
{
  /* any type of ID can be used in custom props. */
  if (owner_id->properties) {
    return FILTER_ID_ALL;
  }
  /* When including UI data (i.e. editors), Screen UI IDs can also link to virtually any ID
   * (through e.g. the Outliner). */
  if (include_ui && GS(owner_id->name) == ID_SCR) {
    return FILTER_ID_ALL;
  }

  /* Casting to non const.
   * TODO(jbakker): We should introduce a ntree_id_has_tree function as we are actually not
   * interested in the result. */
  if (blender::bke::node_tree_from_id(const_cast<ID *>(owner_id))) {
    return FILTER_ID_ALL;
  }

  if (BKE_animdata_from_id(owner_id)) {
    /* AnimationData can use virtually any kind of data-blocks, through drivers especially. */
    return FILTER_ID_ALL;
  }

  if (ID_IS_OVERRIDE_LIBRARY_REAL(owner_id)) {
    /* LibOverride data 'hierarchy root' can virtually point back to any type of ID. */
    return FILTER_ID_ALL;
  }

  if (!owner_id_type) {
    owner_id_type = BKE_idtype_get_info_from_id(owner_id);
  }
  if (owner_id_type) {
    return owner_id_type->dependencies_id_types;
  }
  BLI_assert_unreachable();
  return 0;
}

bool BKE_library_id_can_use_idtype(ID *owner_id, const short id_type_used)
{
  const IDTypeInfo *owner_id_type = BKE_idtype_get_info_from_id(owner_id);
  const uint64_t filter_id_type_used = BKE_idtype_idcode_to_idfilter(id_type_used);
  const uint64_t can_be_used = BKE_library_id_can_use_filter_id(owner_id, false, owner_id_type);
  return (can_be_used & filter_id_type_used) != 0;
}

/* ***** ID users iterator. ***** */
struct IDUsersIter {
  ID *id;

  // ListBase *lb_array[INDEX_ID_MAX]; /* UNUSED. */
  // int lb_idx; /* UNUSED. */

  ID *curr_id;
  int count_direct, count_indirect; /* Set by callback. */
};

static int foreach_libblock_id_users_callback(LibraryIDLinkCallbackData *cb_data)
{
  ID **id_p = cb_data->id_pointer;
  const LibraryForeachIDCallbackFlag cb_flag = cb_data->cb_flag;
  IDUsersIter *iter = static_cast<IDUsersIter *>(cb_data->user_data);

  if (*id_p) {
    /* "Loop-back" ID pointers (the ugly *from* ones, like `Key->from`).
     * Those are not actually ID usage, we can ignore them here.
     */
    if (cb_flag & IDWALK_CB_LOOPBACK) {
      return IDWALK_RET_NOP;
    }

    if (*id_p == iter->id) {
#if 0
      printf(
          "%s uses %s (refcounted: %d, userone: %d, used_one: %d, used_one_active: %d, "
          "indirect_usage: %d)\n",
          iter->curr_id->name,
          iter->id->name,
          (cb_flag & IDWALK_USER) ? 1 : 0,
          (cb_flag & IDWALK_USER_ONE) ? 1 : 0,
          (iter->id->tag & ID_TAG_EXTRAUSER) ? 1 : 0,
          (iter->id->tag & ID_TAG_EXTRAUSER_SET) ? 1 : 0,
          (cb_flag & IDWALK_INDIRECT_USAGE) ? 1 : 0);
#endif
      if (cb_flag & IDWALK_CB_INDIRECT_USAGE) {
        iter->count_indirect++;
      }
      else {
        iter->count_direct++;
      }
    }
  }

  return IDWALK_RET_NOP;
}

int BKE_library_ID_use_ID(ID *id_user, ID *id_used)
{
  IDUsersIter iter;

  /* We do not care about iter.lb_array/lb_idx here... */
  iter.id = id_used;
  iter.curr_id = id_user;
  iter.count_direct = iter.count_indirect = 0;

  BKE_library_foreach_ID_link(
      nullptr, iter.curr_id, foreach_libblock_id_users_callback, (void *)&iter, IDWALK_READONLY);

  return iter.count_direct + iter.count_indirect;
}

static bool library_ID_is_used(Main *bmain, void *idv, const bool check_linked)
{
  IDUsersIter iter;
  MainListsArray lb_array = BKE_main_lists_get(*bmain);
  int i = lb_array.size();
  ID *id = static_cast<ID *>(idv);
  bool is_defined = false;

  iter.id = id;
  iter.count_direct = iter.count_indirect = 0;
  while (i-- && !is_defined) {
    ID *id_curr = static_cast<ID *>(lb_array[i]->first);

    if (!id_curr || !BKE_library_id_can_use_idtype(id_curr, GS(id->name))) {
      continue;
    }

    for (; id_curr && !is_defined; id_curr = static_cast<ID *>(id_curr->next)) {
      if (id_curr == id) {
        /* We are not interested in self-usages (mostly from drivers or bone constraints...). */
        continue;
      }
      iter.curr_id = id_curr;
      BKE_library_foreach_ID_link(
          bmain, id_curr, foreach_libblock_id_users_callback, &iter, IDWALK_READONLY);

      is_defined = ((check_linked ? iter.count_indirect : iter.count_direct) != 0);
    }
  }

  return is_defined;
}

bool BKE_library_ID_is_locally_used(Main *bmain, void *idv)
{
  return library_ID_is_used(bmain, idv, false);
}

bool BKE_library_ID_is_indirectly_used(Main *bmain, void *idv)
{
  return library_ID_is_used(bmain, idv, true);
}

void BKE_library_ID_test_usages(Main *bmain,
                                void *idv,
                                bool *r_is_used_local,
                                bool *r_is_used_linked)
{
  IDUsersIter iter;
  MainListsArray lb_array = BKE_main_lists_get(*bmain);
  int i = lb_array.size();
  ID *id = static_cast<ID *>(idv);
  bool is_defined = false;

  iter.id = id;
  iter.count_direct = iter.count_indirect = 0;
  while (i-- && !is_defined) {
    ID *id_curr = static_cast<ID *>(lb_array[i]->first);

    if (!id_curr || !BKE_library_id_can_use_idtype(id_curr, GS(id->name))) {
      continue;
    }

    for (; id_curr && !is_defined; id_curr = static_cast<ID *>(id_curr->next)) {
      if (id_curr == id) {
        /* We are not interested in self-usages (mostly from drivers or bone constraints...). */
        continue;
      }
      iter.curr_id = id_curr;
      BKE_library_foreach_ID_link(
          bmain, id_curr, foreach_libblock_id_users_callback, &iter, IDWALK_READONLY);

      is_defined = (iter.count_direct != 0 && iter.count_indirect != 0);
    }
  }

  *r_is_used_local = (iter.count_direct != 0);
  *r_is_used_linked = (iter.count_indirect != 0);
}

/* ***** IDs usages.checking/tagging. ***** */

/**
 * Internal data for the common processing of the 'unused IDs' query functions.
 *
 * While #LibQueryUnusedIDsData is a subset of this internal struct, they need to be kept separate,
 * since this struct is used with partially 'enforced' values for some parameters by the
 * #BKE_lib_query_unused_ids_amounts code. This allows the computation of predictive amounts for
 * user feedback ('what would be the amounts of IDs detected as unused if this option was
 * enabled').
 */
struct UnusedIDsData {
  Main *bmain;

  const int id_tag;

  bool do_local_ids;
  bool do_linked_ids;
  bool do_recursive;

  blender::FunctionRef<bool(ID *id)> filter_fn;

  std::array<int, INDEX_ID_MAX> *num_total;
  std::array<int, INDEX_ID_MAX> *num_local;
  std::array<int, INDEX_ID_MAX> *num_linked;

  blender::Set<ID *> unused_ids;

  UnusedIDsData(Main *bmain, const int id_tag, LibQueryUnusedIDsData &parameters)
      : bmain(bmain),
        id_tag(id_tag),
        do_local_ids(parameters.do_local_ids),
        do_linked_ids(parameters.do_linked_ids),
        do_recursive(parameters.do_recursive),
        filter_fn(parameters.filter_fn),
        num_total(&parameters.num_total),
        num_local(&parameters.num_local),
        num_linked(&parameters.num_linked)
  {
  }

  void reset(const bool do_local_ids,
             const bool do_linked_ids,
             const bool do_recursive,
             std::array<int, INDEX_ID_MAX> &num_total,
             std::array<int, INDEX_ID_MAX> &num_local,
             std::array<int, INDEX_ID_MAX> &num_linked)
  {
    unused_ids.clear();
    this->do_local_ids = do_local_ids;
    this->do_linked_ids = do_linked_ids;
    this->do_recursive = do_recursive;
    this->num_total = &num_total;
    this->num_local = &num_local;
    this->num_linked = &num_linked;
  }
};

static void lib_query_unused_ids_tag_id(ID *id, UnusedIDsData &data)
{
  if (data.filter_fn && !data.filter_fn(id)) {
    return;
  }
  id->tag |= data.id_tag;
  data.unused_ids.add(id);

  const int id_code = BKE_idtype_idcode_to_index(GS(id->name));
  (*data.num_total)[INDEX_ID_NULL]++;
  (*data.num_total)[id_code]++;
  if (ID_IS_LINKED(id)) {
    (*data.num_linked)[INDEX_ID_NULL]++;
    (*data.num_linked)[id_code]++;
  }
  else {
    (*data.num_local)[INDEX_ID_NULL]++;
    (*data.num_local)[id_code]++;
  }
}

static void lib_query_unused_ids_untag_id(ID &id, UnusedIDsData &data)
{
  BLI_assert(data.unused_ids.contains(&id));

  id.tag &= ~data.id_tag;
  data.unused_ids.remove_contained(&id);

  const int id_code = BKE_idtype_idcode_to_index(GS(id.name));
  (*data.num_total)[INDEX_ID_NULL]--;
  (*data.num_total)[id_code]--;
  if (ID_IS_LINKED(&id)) {
    (*data.num_linked)[INDEX_ID_NULL]--;
    (*data.num_linked)[id_code]--;
  }
  else {
    (*data.num_local)[INDEX_ID_NULL]--;
    (*data.num_local)[id_code]--;
  }
}

/**
 * Certain corner-cases require to consider an ID as used,
 * even if there are no 'real' reference-counting usages of these.
 */
static bool lib_query_unused_ids_has_exception_user(ID &id, UnusedIDsData &data)
{
  switch (GS(id.name)) {
    case ID_OB: {
      /* FIXME: This is a workaround until Object usages are handled more soundly.
       *
       * Historically, only reference-counting Object usages were the Collection ones. All other
       * references (e.g. as Constraints or Modifiers targets) did not increase their user-count.
       *
       * This is not entirely true anymore (e.g. some type-agnostic ID usages like IDPointer custom
       * properties do refcount Object ones too), but there are still many Object usages that
       * should refcount them and don't do it.
       *
       * This becomes a problem with linked data, as in that case instancing of linked Objects in
       * the scene is not enforced (to avoid cluttering the scene), which leaves some actually used
       * linked objects with a `0` user-count.
       *
       * So this is a special check to consider linked objects as used also in case some other
       * used ID uses them.
       */
      if (!ID_IS_LINKED(&id)) {
        return false;
      }
      MainIDRelationsEntry *id_relations = static_cast<MainIDRelationsEntry *>(
          BLI_ghash_lookup(data.bmain->relations->relations_from_pointers, &id));
      for (MainIDRelationsEntryItem *from = id_relations->from_ids; from; from = from->next) {
        if (!data.unused_ids.contains(from->id_pointer.from)) {
          return true;
        }
      }
      break;
    }
    case ID_IM: {
      /* Images which have a 'viewer' source (e.g. render results) should not be considered as
       * orphaned/unused data. */
      const Image &image = reinterpret_cast<Image &>(id);
      if (image.source == IMA_SRC_VIEWER) {
        return true;
      }
      break;
    }
    default:
      return false;
  }
  return false;
}

/**
 * Returns `true` if given ID is detected as part of at least one dependency loop, false otherwise.
 */
static bool lib_query_unused_ids_tag_recurse(ID *id, UnusedIDsData &data)
{
  /* We should never deal with embedded, not-in-main IDs here. */
  BLI_assert((id->flag & ID_FLAG_EMBEDDED_DATA) == 0);

  MainIDRelationsEntry *id_relations = static_cast<MainIDRelationsEntry *>(
      BLI_ghash_lookup(data.bmain->relations->relations_from_pointers, id));

  if ((id_relations->tags & MAINIDRELATIONS_ENTRY_TAGS_PROCESSED) != 0) {
    return false;
  }
  if ((id_relations->tags & MAINIDRELATIONS_ENTRY_TAGS_INPROGRESS) != 0) {
    /* This ID has not yet been fully processed. If this condition is reached, it means this is a
     * dependency loop case. */
    return true;
  }

  if ((!data.do_linked_ids && ID_IS_LINKED(id)) || (!data.do_local_ids && !ID_IS_LINKED(id))) {
    id_relations->tags |= MAINIDRELATIONS_ENTRY_TAGS_PROCESSED;
    return false;
  }

  if (data.unused_ids.contains(id)) {
    id_relations->tags |= MAINIDRELATIONS_ENTRY_TAGS_PROCESSED;
    return false;
  }

  if ((id->flag & ID_FLAG_FAKEUSER) != 0) {
    /* This ID is forcefully kept around, and therefore never unused, no need to check it further.
     */
    id_relations->tags |= MAINIDRELATIONS_ENTRY_TAGS_PROCESSED;
    return false;
  }

  const IDTypeInfo *id_type = BKE_idtype_get_info_from_id(id);
  if (id_type->flags & IDTYPE_FLAGS_NEVER_UNUSED) {
    /* Some 'root' ID types are never unused (even though they may not have actual users), unless
     * their actual user-count is set to 0. */
    id_relations->tags |= MAINIDRELATIONS_ENTRY_TAGS_PROCESSED;
    return false;
  }

  if (lib_query_unused_ids_has_exception_user(*id, data)) {
    id_relations->tags |= MAINIDRELATIONS_ENTRY_TAGS_PROCESSED;
    return false;
  }

  /* An ID user is 'valid' (i.e. may affect the 'used'/'not used' status of the ID it uses) if it
   * does not match `ignored_usages`, and does match `required_usages`. */
  const int ignored_usages = (IDWALK_CB_LOOPBACK | IDWALK_CB_EMBEDDED |
                              IDWALK_CB_EMBEDDED_NOT_OWNING);
  const int required_usages = (IDWALK_CB_USER | IDWALK_CB_USER_ONE);

  /* This ID may be tagged as unused if none of its users are 'valid', as defined above.
   *
   * First recursively check all its valid users, if all of them can be tagged as
   * unused, then we can tag this ID as such too. */
  bool has_valid_from_users = false;
  bool is_part_of_dependency_loop = false;
  id_relations->tags |= MAINIDRELATIONS_ENTRY_TAGS_INPROGRESS;
  for (MainIDRelationsEntryItem *id_from_item = id_relations->from_ids; id_from_item != nullptr;
       id_from_item = id_from_item->next)
  {
    if ((id_from_item->usage_flag & ignored_usages) != 0 ||
        (id_from_item->usage_flag & required_usages) == 0)
    {
      continue;
    }

    ID *id_from = id_from_item->id_pointer.from;
    if ((id_from->flag & ID_FLAG_EMBEDDED_DATA) != 0) {
      /* Directly 'by-pass' to actual real ID owner. */
      id_from = BKE_id_owner_get(id_from);
      BLI_assert(id_from != nullptr);
    }

    if (lib_query_unused_ids_tag_recurse(id_from, data)) {
      /* Dependency loop case, ignore the `id_from` tag value here (as it should not be considered
       * as valid yet), and presume that this is a 'valid user' case for now. */
      is_part_of_dependency_loop = true;
      continue;
    }
    if (!data.unused_ids.contains(id_from)) {
      has_valid_from_users = true;
      break;
    }
  }
  if (!has_valid_from_users && !is_part_of_dependency_loop) {
    /* Tag the ID as unused, only in case it is not part of a dependency loop. */
    lib_query_unused_ids_tag_id(id, data);
  }

  /* This ID is not being processed anymore.
   *
   * However, we can only tag is as successfully processed if either it was detected as part of a
   * valid usage hierarchy, or, if detected as unused, if it was not part of a dependency loop.
   *
   * Otherwise, this is an undecided state, it will be resolved at the entry point of this
   * recursive process for the root id (see below in  #BKE_lib_query_unused_ids_tag calling code).
   */
  id_relations->tags &= ~MAINIDRELATIONS_ENTRY_TAGS_INPROGRESS;
  if (has_valid_from_users || !is_part_of_dependency_loop) {
    id_relations->tags |= MAINIDRELATIONS_ENTRY_TAGS_PROCESSED;
  }

  /* If that ID is part of a dependency loop, but it does have a valid user (which is not part of
   * that loop), then that dependency loop does not form (or is not part of) an unused archipelago.
   *
   * In other words, this current `id` is used, and is therefore a valid user of the 'calling ID'
   * from previous recursion level.. */
  return is_part_of_dependency_loop && !has_valid_from_users;
}

static void lib_query_unused_ids_tag(UnusedIDsData &data)
{
  BLI_assert(data.bmain->relations != nullptr);
  BKE_main_relations_tag_set(data.bmain, MAINIDRELATIONS_ENTRY_TAGS_PROCESSED, false);

  /* First loop, to only check for immediately unused IDs (those with 0 user count).
   * NOTE: It also takes care of clearing given tag for used IDs. */
  ID *id;
  FOREACH_MAIN_ID_BEGIN (data.bmain, id) {
    if ((!data.do_linked_ids && ID_IS_LINKED(id)) || (!data.do_local_ids && !ID_IS_LINKED(id))) {
      id->tag &= ~data.id_tag;
    }
    else if (id->us == 0) {
      lib_query_unused_ids_tag_id(id, data);
    }
    else {
      id->tag &= ~data.id_tag;
    }
  }
  FOREACH_MAIN_ID_END;

  /* Special post-process to handle linked objects with no users, see
   * #lib_query_unused_ids_has_exception_user for details.
   *
   * NOTE: Here needs to be in a separate loop, so that all directly unused users of objects have
   * been tagged as such already by the previous loop. */
  constexpr int max_loop_num = 10;
  int loop_num;
  for (loop_num = 0; loop_num < max_loop_num; loop_num++) {
    bool do_loop = false;
    FOREACH_MAIN_LISTBASE_ID_BEGIN (&data.bmain->objects, id) {
      if (!data.unused_ids.contains(id)) {
        continue;
      }
      if (lib_query_unused_ids_has_exception_user(*id, data)) {
        lib_query_unused_ids_untag_id(*id, data);
        do_loop = true;
      }
    }
    FOREACH_MAIN_LISTBASE_ID_END;
    if (!do_loop) {
      break;
    }
  }
  if (loop_num >= max_loop_num) {
    CLOG_WARN(&LOG, "Unexpected levels of dependencies between non-instantiated but used Objects");
  }

  if (!data.do_recursive) {
    return;
  }

  FOREACH_MAIN_ID_BEGIN (data.bmain, id) {
    if (lib_query_unused_ids_tag_recurse(id, data)) {
      /* This root processed ID is part of one or more dependency loops.
       *
       * If it was not tagged, and its matching relations entry is not marked as processed, it
       * means that it's the first encountered entry point of an 'unused archipelago' (i.e. the
       * entry point to a set of IDs with relationships to each other, but no 'valid usage'
       * relations to the current Blender file (like being part of a scene, etc.).
       *
       * So the entry can be tagged as processed, and the ID tagged as unused. */
      if (!data.unused_ids.contains(id)) {
        MainIDRelationsEntry *id_relations = static_cast<MainIDRelationsEntry *>(
            BLI_ghash_lookup(data.bmain->relations->relations_from_pointers, id));
        if ((id_relations->tags & MAINIDRELATIONS_ENTRY_TAGS_PROCESSED) == 0) {
          id_relations->tags |= MAINIDRELATIONS_ENTRY_TAGS_PROCESSED;
          lib_query_unused_ids_tag_id(id, data);
        }
      }
    }

#ifndef NDEBUG
    /* Relation entry for the root processed ID should always be marked as processed now. */
    MainIDRelationsEntry *id_relations = static_cast<MainIDRelationsEntry *>(
        BLI_ghash_lookup(data.bmain->relations->relations_from_pointers, id));
    BLI_assert((id_relations->tags & MAINIDRELATIONS_ENTRY_TAGS_PROCESSED) != 0);
    BLI_assert((id_relations->tags & MAINIDRELATIONS_ENTRY_TAGS_INPROGRESS) == 0);
#endif
  }
  FOREACH_MAIN_ID_END;
}

void BKE_lib_query_unused_ids_amounts(Main *bmain, LibQueryUnusedIDsData &parameters)
{
  std::array<int, INDEX_ID_MAX> num_dummy{0};
  BKE_main_relations_create(bmain, 0);

  parameters.num_total.fill(0);
  parameters.num_local.fill(0);
  parameters.num_linked.fill(0);

  /* The complex fiddling with the two calls, which data they each get, based on the `do_local_ids`
   * and `do_linked_ids`, is here to reduce as much as possible the extra processing:
   *
   * If both local and linked options are enabled, a single call with all given parameters gives
   * all required data about unused IDs.
   *
   * If both local and linked options are disabled, total amount is left at zero, and each local
   * and linked amounts are computed separately.
   *
   * If local is disabled and linked is enabled, the first call will compute the amount of local
   * IDs that would be unused if the local option was enabled. Therefore, only the local amount can
   * be kept from this call. The second call will compute valid values for both linked, and total
   * data.
   *
   * If local is enabled and linked is disabled, the first call will compute valid values for both
   * local, and total data. The second call will compute the amount of linked IDs that would be
   * unused if the linked option was enabled. Therefore, only the linked amount can be kept from
   * this call.
   */

  UnusedIDsData data(bmain, 0, parameters);
  data.do_local_ids = true;
  if (!parameters.do_local_ids) {
    data.num_total = &num_dummy;
  }
  if (!(parameters.do_local_ids && parameters.do_linked_ids)) {
    data.num_linked = &num_dummy;
  }
  lib_query_unused_ids_tag(data);

  if (!(parameters.do_local_ids && parameters.do_linked_ids)) {
    /* In case a second run is required, clear runtime data and update settings for linked data. */
    data.reset(parameters.do_local_ids,
               true,
               parameters.do_recursive,
               (!parameters.do_local_ids && parameters.do_linked_ids) ? parameters.num_total :
                                                                        num_dummy,
               num_dummy,
               parameters.num_linked);
    lib_query_unused_ids_tag(data);
  }

  BKE_main_relations_free(bmain);
}

void BKE_lib_query_unused_ids_tag(Main *bmain, const int tag, LibQueryUnusedIDsData &parameters)
{
  BLI_assert(tag != 0);

  parameters.num_total.fill(0);
  parameters.num_local.fill(0);
  parameters.num_linked.fill(0);

  UnusedIDsData data(bmain, tag, parameters);

  BKE_main_relations_create(bmain, 0);
  lib_query_unused_ids_tag(data);
  BKE_main_relations_free(bmain);
}

static int foreach_libblock_used_linked_data_tag_clear_cb(LibraryIDLinkCallbackData *cb_data)
{
  ID *self_id = cb_data->self_id;
  ID **id_p = cb_data->id_pointer;
  const LibraryForeachIDCallbackFlag cb_flag = cb_data->cb_flag;
  bool *is_changed = static_cast<bool *>(cb_data->user_data);

  if (*id_p) {
    /* The infamous 'from' pointers (Key.from, ...).
     * those are not actually ID usage, so we ignore them here. */
    if (cb_flag & IDWALK_CB_LOOPBACK) {
      return IDWALK_RET_NOP;
    }

    /* If checked id is used by an assumed used ID,
     * then it is also used and not part of any linked archipelago. */
    if (!(self_id->tag & ID_TAG_DOIT) && ((*id_p)->tag & ID_TAG_DOIT)) {
      (*id_p)->tag &= ~ID_TAG_DOIT;
      *is_changed = true;
    }
  }

  return IDWALK_RET_NOP;
}

void BKE_library_unused_linked_data_set_tag(Main *bmain, const bool do_init_tag)
{
  ID *id;

  if (do_init_tag) {
    FOREACH_MAIN_ID_BEGIN (bmain, id) {
      if (id->lib && (id->tag & ID_TAG_INDIRECT) != 0) {
        id->tag |= ID_TAG_DOIT;
      }
      else {
        id->tag &= ~ID_TAG_DOIT;
      }
    }
    FOREACH_MAIN_ID_END;
  }

  for (bool do_loop = true; do_loop;) {
    do_loop = false;
    FOREACH_MAIN_ID_BEGIN (bmain, id) {
      /* We only want to check that ID if it is currently known as used... */
      if ((id->tag & ID_TAG_DOIT) == 0) {
        BKE_library_foreach_ID_link(
            bmain, id, foreach_libblock_used_linked_data_tag_clear_cb, &do_loop, IDWALK_READONLY);
      }
    }
    FOREACH_MAIN_ID_END;
  }
}

void BKE_library_indirectly_used_data_tag_clear(Main *bmain)
{
  bool do_loop = true;
  while (do_loop) {
    MainListsArray lb_array = BKE_main_lists_get(*bmain);
    int i = lb_array.size();
    do_loop = false;

    while (i--) {
      LISTBASE_FOREACH (ID *, id, lb_array[i]) {
        if (!ID_IS_LINKED(id) || id->tag & ID_TAG_DOIT) {
          /* Local or non-indirectly-used ID (so far), no need to check it further. */
          continue;
        }
        BKE_library_foreach_ID_link(
            bmain, id, foreach_libblock_used_linked_data_tag_clear_cb, &do_loop, IDWALK_READONLY);
      }
    }
  }
}

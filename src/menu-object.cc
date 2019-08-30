#include "menu-object.hh"

struct MENUEX_TEMPLATE_HEADER {
  uint16_t version;
  uint16_t offset;
  uint32_t helpid;
};

struct MENUEX_TEMPLATE_ITEM {
  uint32_t type;
  uint32_t state;
  uint32_t id;
  uint16_t flags;
  wchar_t text[1];  // actual length is dynamic

  //   if (items) {
  //       ?? uint32 helpid;
  //       ?? MENUEX_TEMPLATE_ITEM[...] items;
  //   }
};

void* write_text(std::wstring_view src, MENUEX_TEMPLATE_ITEM* item) {
  // need cast to defeat iterator range check on text, since it's actually
  // dynamically sized.
  auto ptr = std::copy(begin(src), end(src), (wchar_t*)item->text);
  *ptr++ = L'\0';      // terminator
  if (src.size() % 2)  // odd number of wchar_t's, not including terminator?
    *ptr++ = L'\0';    // add padding, so total item size is multiple of 4 bytes
  return ptr;
}

struct menu_item {
  std::optional<int32_t> id;
  std::optional<std::wstring> text;
  std::optional<bool> separator = false;
  std::optional<bool> disabled = false;
  std::optional<bool> checked = false;
  std::optional<std::vector<menu_item>> items;

  static size_t template_size(std::vector<menu_item> const& items) {
    if (items.empty()) {
      // Size of an item containing "Empty"
      return template_item_size(5);
    }

    size_t size = 0;
    for (auto& item : items) {
      size += item.template_size();
    }
    return size;
  }

  static size_t template_item_size(size_t text_chars) {
    // If there are an odd number of text chars, then add an extra 2 bytes
    // after the terminator so the total size is a multiple of 4 bytes.
    auto size = 14 + text_chars * 2 + (text_chars % 2 ? 4 : 2);
    return size;
  }

  size_t template_size() const {
    auto size = template_item_size(text.has_value() ? text.value().size() : 0);
    if (items) {
      size += 4 + template_size(items.value());
    }
    return size;
  }

  static void* write_template(std::vector<menu_item> const& items,
                              void* output) {
    if (items.empty()) {
      auto output_item = (MENUEX_TEMPLATE_ITEM*)output;
      output_item->state |= MFS_DISABLED;
      output_item->flags |= MF_END;
      return write_text(L"Empty"sv, output_item);
    }
    auto last = end(items);
    --last;

    for (auto it = begin(items); it != last; ++it) {
      output = it->write_template((MENUEX_TEMPLATE_ITEM*)output, false);
    }
    return last->write_template((MENUEX_TEMPLATE_ITEM*)output, true);
  }

  void* write_template(MENUEX_TEMPLATE_ITEM* output, bool is_last) const {
    output->type = 0;
    if (separator.value_or(false)) output->type |= MFT_SEPARATOR;
    output->state = 0;
    if (disabled.value_or(false)) output->state |= MFS_DISABLED;
    if (checked.value_or(false)) output->state |= MFS_CHECKED;
    output->id = id.value_or(0);
    output->flags = 0;
    if (is_last) output->flags |= MF_END;
    if (items) output->flags |= 0x01;  // doesn't seem to have a definition
    auto end = (char*)(text ? write_text(text.value(), output)
                            : write_text(L""sv, output));
    if (items) {
      end += 4;
      return write_template(items.value(), end);
    }
    return end;
  }

  void update_item_info(MENUITEMINFO* item) {
    item->fMask = 0;
    if (separator) {
      item->fMask |= MIIM_FTYPE;
      item->fType = MFT_SEPARATOR;
    }
    if (disabled) {
      item->fMask |= MIIM_STATE;
      item->fState &= ~MFS_DISABLED;
      if (disabled.value()) item->fState |= MFS_DISABLED;
    }
    if (checked) {
      item->fMask |= MIIM_STATE;
      item->fState &= ~MFS_CHECKED;
      if (checked.value()) item->fState |= MFS_CHECKED;
    }
    if (id) {
      item->fMask |= MIIM_ID;
      item->wID = id.value();
    }
    if (text) {
      item->fMask |= MIIM_STRING;
      item->dwTypeData = const_cast<LPWSTR>(text.value().c_str());
    }
  }
};

napi_status napi_get_value(napi_env env, napi_value value, menu_item* result) {
  NAPI_RETURN_IF_NOT_OK(napi_get_named_property(env, value, "id", &result->id));
  NAPI_RETURN_IF_NOT_OK(
      napi_get_named_property(env, value, "text", &result->text));
  NAPI_RETURN_IF_NOT_OK(
      napi_get_named_property(env, value, "separator", &result->separator));
  NAPI_RETURN_IF_NOT_OK(
      napi_get_named_property(env, value, "disabled", &result->disabled));
  NAPI_RETURN_IF_NOT_OK(
      napi_get_named_property(env, value, "checked", &result->checked));
  NAPI_RETURN_IF_NOT_OK(
      napi_get_named_property(env, value, "items", &result->items));
  return napi_ok;
}

static MenuHandle load_menu_indirect(napi_env env, const void* data) {
  MenuHandle menu = LoadMenuIndirectW(data);
  if (!menu) {
    napi_throw_win32_error(env, "LoadMenuIndirectW");
    return nullptr;
  }

  MenuHandle submenu = GetSubMenu(menu, 0);
  if (!submenu) {
    napi_throw_win32_error(env, "GetSubMenu");
    return nullptr;
  }

  if (!RemoveMenu(menu, 0, MF_BYPOSITION)) {
    napi_throw_win32_error(env, "RemoveMenu");
    return nullptr;
  }

  return submenu;
}

static MenuHandle create_menu(napi_env env, std::vector<menu_item> items) {
  // Need to wrap the actual items in a dummy menu item, so the actual items are
  // in a popup menu. load_menu_indirect() will then unwrap the first item back
  // out.
  menu_item dummy_item;
  dummy_item.text = L"root";
  dummy_item.items = std::move(items);

  auto size = 8 + dummy_item.template_size();
  auto data = std::make_unique<char[]>(size);
  memset(data.get(), 0, size);

  {
    auto header = (MENUEX_TEMPLATE_HEADER*)data.get();
    header->version = 1;
    header->offset = 4;
    dummy_item.write_template((MENUEX_TEMPLATE_ITEM*)(header + 1), true);
  }

  return load_menu_indirect(env, data.get());
}

static napi_value wrap_menu(napi_env env, MenuHandle menu) {
  if (!menu) return nullptr;

  auto env_data = get_env_data(env);

  napi_value result;
  NAPI_THROW_RETURN_NULL_IF_NOT_OK(
      env, MenuObject::new_instance(env_data, std::move(menu), &result));
  return result;
}

napi_value export_Menu_create(napi_env env, napi_callback_info info) {
  std::vector<menu_item> items;
  NAPI_RETURN_NULL_IF_NOT_OK(napi_get_required_args(env, info, &items));

  return wrap_menu(env, create_menu(env, std::move(items)));
}

napi_value export_Menu_createFromTemplate(napi_env env,
                                          napi_callback_info info) {
  napi_value value;
  NAPI_RETURN_NULL_IF_NOT_OK(napi_get_required_args(env, info, &value));

  void* data = nullptr;
  size_t length = 0;
  NAPI_THROW_RETURN_NULL_IF_NOT_OK(
      env, napi_get_buffer_info(env, value, &data, &length));

  return wrap_menu(env, load_menu_indirect(env, data));
}

napi_value export_Menu_show(napi_env env, napi_callback_info info) {
  MenuObject* this_object;
  int32_t mouse_x;
  int32_t mouse_y;
  NAPI_RETURN_NULL_IF_NOT_OK(napi_get_cb_info(env, info, &this_object, nullptr,
                                              2, &mouse_x, &mouse_y));

  auto env_data = get_env_data(env);
  if (!env_data) {
    return nullptr;
  }

  HMENU menu = this_object->menu;

  napi_deferred deferred;
  napi_value promise;
  NAPI_THROW_RETURN_NULL_IF_NOT_OK(
      env, napi_create_promise(env, &deferred, &promise));

  env_data->icon_message_loop.run_on_msg_thread_nonblocking([=] {
    auto env_data = get_env_data(env);
    if (!env_data) {
      return;
    }

    int32_t item_id = 0;
    DWORD error = 0;
    item_id = (int32_t)TrackPopupMenuEx(
        menu,
        GetSystemMetrics(SM_MENUDROPALIGNMENT) | TPM_RETURNCMD | TPM_NONOTIFY,
        mouse_x, mouse_y, env_data->icon_message_loop.hwnd, nullptr);
    if (!item_id) {
      error = GetLastError();
    }

    env_data->icon_message_loop.run_on_env_thread.blocking(
        [=](napi_env env, napi_value) {
          if (error) {
            napi_value error_value;
            NAPI_THROW_RETURN_VOID_IF_NOT_OK(
                env, napi_create_win32_error(env, "TrackPopupMenuEx", error,
                                             &error_value));
            NAPI_THROW_RETURN_VOID_IF_NOT_OK(
                env, napi_reject_deferred(env, deferred, error_value));
          } else {
            napi_value result;
            NAPI_THROW_RETURN_VOID_IF_NOT_OK(
                env, item_id ? napi_create(env, item_id, &result)
                             : napi_get_null(env, &result));
            NAPI_THROW_RETURN_VOID_IF_NOT_OK(
                env, napi_resolve_deferred(env, deferred, result));
          }
        });
  });

  return promise;
}

napi_value export_Menu_showSync(napi_env env, napi_callback_info info) {
  MenuObject* this_object;
  int32_t mouse_x;
  int32_t mouse_y;
  NAPI_RETURN_NULL_IF_NOT_OK(napi_get_cb_info(env, info, &this_object, nullptr,
                                              2, &mouse_x, &mouse_y));

  auto env_data = get_env_data(env);

  HMENU menu = this_object->menu;

  int32_t item_id = 0;
  DWORD error = 0;

  env_data->icon_message_loop.run_on_msg_thread_blocking([=, &item_id, &error] {
    item_id = (int32_t)TrackPopupMenuEx(
        menu,
        GetSystemMetrics(SM_MENUDROPALIGNMENT) | TPM_RETURNCMD | TPM_NONOTIFY,
        mouse_x, mouse_y, env_data->icon_message_loop.hwnd, nullptr);
    if (!item_id) {
      error = GetLastError();
    }
  });

  if (error) {
    napi_throw_win32_error(env, "TrackPopupMenuEx", error);
    return nullptr;
  }
  napi_value result;
  NAPI_THROW_RETURN_NULL_IF_NOT_OK(env, item_id
                                            ? napi_create(env, item_id, &result)
                                            : napi_get_null(env, &result));
  return result;
}

napi_value get_menu_item(napi_env env, HMENU menu, int32_t item_id_or_index,
                         bool by_index) {
  std::optional<std::wstring> text;

  MENUITEMINFOW item = {sizeof(item)};
  item.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE;
  if (!GetMenuItemInfoW(menu, item_id_or_index, by_index, &item)) {
    napi_throw_win32_error(env, "GetMenuItemInfoW");
    return nullptr;
  }
  if (item.fType == MFT_STRING) {
    item.fMask |= MIIM_STRING;
    // fails with invalid parameter, as we didn't set dwTypeData
    GetMenuItemInfoW(menu, item_id_or_index, by_index, &item);

    text.emplace();
    text->resize(item.cch);
    ++item.cch;
    item.dwTypeData = text->data();
    if (!GetMenuItemInfoW(menu, item_id_or_index, by_index, &item)) {
      napi_throw_win32_error(env, "GetMenuItemInfoW");
      return nullptr;
    }
  }

  napi_value item_value;
  NAPI_THROW_RETURN_NULL_IF_NOT_OK(
      env,
      napi_create_object(env, &item_value,
                         {
                             {"id", item.wID},
                             {"text", text},
                             {"separator", (item.fType & MFT_SEPARATOR) != 0},
                             {"disabled", (item.fState & MFS_DISABLED) != 0},
                             {"checked", (item.fState & MFS_CHECKED) != 0},
                         }));

  return item_value;
}

napi_value update_menu_item(napi_env env, HMENU menu, int32_t item_id_or_value,
                            bool by_index, menu_item options) {
  MENUITEMINFOW item = {sizeof(item)};

  // So we can update individual flags.
  if (!GetMenuItemInfoW(menu, item_id_or_value, by_index, &item)) {
    napi_throw_win32_error(env, "GetMenuItemInfoW");
    return nullptr;
  }

  options.update_item_info(&item);

  MenuHandle items_menu;
  if (options.items) {
    item.fMask |= MIIM_SUBMENU;
    items_menu = create_menu(env, std::move(options.items.value()));
    item.hSubMenu = items_menu;
  }

  if (!SetMenuItemInfoW(menu, item_id_or_value, by_index, &item)) {
    napi_throw_win32_error(env, "SetMenuItemInfoW");
    return nullptr;
  }
  // Now it's owned by menu
  items_menu.release();

  return nullptr;
}

napi_value export_Menu_getAt(napi_env env, napi_callback_info info) {
  MenuObject* this_object;
  int32_t index;
  NAPI_RETURN_NULL_IF_NOT_OK(
      napi_get_cb_info(env, info, &this_object, nullptr, 1, &index));

  return get_menu_item(env, this_object->menu, index, true);
}

napi_value export_Menu_get(napi_env env, napi_callback_info info) {
  MenuObject* this_object;
  int32_t item_id;
  NAPI_RETURN_NULL_IF_NOT_OK(
      napi_get_cb_info(env, info, &this_object, nullptr, 1, &item_id));

  return get_menu_item(env, this_object->menu, item_id, false);
}

napi_value export_Menu_updateAt(napi_env env, napi_callback_info info) {
  MenuObject* this_object;
  int32_t index;
  menu_item options;
  NAPI_RETURN_NULL_IF_NOT_OK(
      napi_get_cb_info(env, info, &this_object, nullptr, 2, &index, &options));

  return update_menu_item(env, this_object->menu, index, true,
                          std::move(options));
}

napi_value export_Menu_update(napi_env env, napi_callback_info info) {
  MenuObject* this_object;
  int32_t item_id;
  menu_item options;
  NAPI_RETURN_NULL_IF_NOT_OK(napi_get_cb_info(env, info, &this_object, nullptr,
                                              2, &item_id, &options));

  return update_menu_item(env, this_object->menu, item_id, false,
                          std::move(options));
}

auto MenuObject::define_class(EnvData* env_data, napi_value* constructor_value)
    -> napi_status {
  return NapiWrapped::define_class(
      env_data->env, "Menu", constructor_value, &env_data->menu_constructor,
      {
          napi_method_property("show", export_Menu_show),
          napi_method_property("showSync", export_Menu_showSync),
          napi_method_property("getAt", export_Menu_getAt),
          napi_method_property("get", export_Menu_get),
          napi_method_property("updateAt", export_Menu_updateAt),
          napi_method_property("update", export_Menu_update),
      });
}

napi_status MenuObject::new_instance(EnvData* env_data, MenuHandle menu,
                                     napi_value* result) {
  NAPI_RETURN_IF_NOT_OK(NapiWrapped::new_instance(
      env_data->env, env_data->menu_constructor, result));
  MenuObject* wrapped = nullptr;
  NAPI_RETURN_IF_NOT_OK(
      NapiWrapped::try_unwrap(env_data->env, *result, &wrapped));
  wrapped->menu = std::move(menu);
  return napi_ok;
}

napi_status MenuObject::init(napi_env env, napi_callback_info info,
                             napi_value* result) {
  napi_value value;
  NAPI_RETURN_IF_NOT_OK(
      napi_get_cb_info(env, info, result, nullptr, 1, &value));

  bool is_array;
  NAPI_RETURN_IF_NOT_OK(napi_is_array(env, value, &is_array));
  if (is_array) {
    std::vector<menu_item> items;
    NAPI_RETURN_IF_NOT_OK(napi_get_value(env, value, &items));
    menu = create_menu(env, std::move(items));
  } else {
    void* data = nullptr;
    size_t size = 0;
    NAPI_RETURN_IF_NOT_OK(napi_get_buffer_info(env, value, &data, &size));
    menu = load_menu_indirect(env, data);
  }
  return napi_ok;
}
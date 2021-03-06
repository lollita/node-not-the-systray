#pragma once

#include "napi/core.hh"
#include "napi/ref.hh"

template <typename T>
struct NapiUnwrappedRef : NapiRef {
  T* wrapped;
};

struct NapiWrappedTypeObject {};

template <typename T>
struct NapiWrapped {
  // Checks unwrap is safe by comparing a header pointer to this object
  inline static NapiWrappedTypeObject type_object;

  struct TypeWrapper {
    NapiWrappedTypeObject* type_ptr = &type_object;
    T value;
  };

  static napi_status define_class(
      napi_env env, std::string_view name, napi_ref* constructor_ref,
      std::initializer_list<napi_property_descriptor> static_props = {}) {
    napi_value constructor_value;
    return define_class(env, name, &constructor_value, constructor_ref,
                        static_props);
  }

  static napi_status define_class(
      napi_env env, std::string_view name, napi_value* constructor_value,
      napi_ref* constructor_ref,
      std::initializer_list<napi_property_descriptor> static_props = {}) {
    NAPI_RETURN_IF_NOT_OK(napi_define_class(
        env, name.data(), name.size(), constructor, nullptr,
        static_props.size(), static_props.begin(), constructor_value));
    return napi_create_reference(env, *constructor_value, 1, constructor_ref);
  }

  static napi_value constructor(napi_env env, napi_callback_info info) {
    napi_value this_value = nullptr;
    auto ptr = std::make_unique<TypeWrapper>();
    NAPI_RETURN_NULL_IF_NOT_OK(ptr->value.init(env, info, &this_value));
    NAPI_RETURN_NULL_IF_NOT_OK(
        napi_wrap(env, this_value, ptr.get(), finalize, nullptr, nullptr));
    ptr.release();
    return this_value;
  }

  static void finalize(napi_env env, void* finalize_data, void* finalize_hint) {
    delete (TypeWrapper*)finalize_data;
  }

  static napi_status new_instance(napi_env env, napi_ref constructor_ref,
                                  napi_value* result,
                                  std::initializer_list<napi_value> args = {}) {
    napi_value constructor = nullptr;
    NAPI_RETURN_IF_NOT_OK(
        napi_get_reference_value(env, constructor_ref, &constructor));
    return napi_new_instance(env, constructor, args.size(), args.begin(),
                             result);
  }

  static napi_status new_instance(napi_env env, napi_value constructor,
                                  napi_value* result,
                                  std::initializer_list<napi_value> args = {}) {
    return napi_new_instance(env, constructor, args.size(), args.begin(),
                             result);
  }

  static napi_status try_unwrap(napi_env env, napi_value value, T** result) {
    void* raw = nullptr;
    NAPI_RETURN_IF_NOT_OK(napi_unwrap(env, value, &raw));
    auto typed = static_cast<TypeWrapper*>(raw);
    *result = typed->type_ptr != &type_object ? nullptr : &typed->value;
    return napi_ok;
  }

  using Ref = NapiUnwrappedRef<T>;

  static napi_status try_create_ref(napi_env env, napi_value value,
                                    Ref* result) {
    if (auto status = try_unwrap(env, value, &result->wrapped);
        !result->wrapped) {
      return status;
    }
    NAPI_RETURN_IF_NOT_OK(result->create(env, value));
    return napi_ok;
  }

 protected:
  napi_status init(napi_env env, napi_callback_info info, napi_value* result) {
    return napi_get_cb_info(env, info, nullptr, nullptr, result, nullptr);
  }

  NapiWrapped() = default;
  NapiWrapped(NapiWrapped const&) = delete;
  NapiWrapped(NapiWrapped&&) = delete;
  NapiWrapped& operator=(NapiWrapped const&) = delete;
  NapiWrapped& operator=(NapiWrapped&&) = delete;
};

template <typename T>
napi_status napi_get_value(napi_env env, napi_value value,
                           NapiUnwrappedRef<T>* result) {
  NAPI_RETURN_IF_NOT_OK(NapiWrapped<T>::try_create_ref(env, value, result));
  if (!result->wrapped) {
    napi_throw_type_error(env, nullptr, "Invalid native object type");
    return napi_pending_exception;
  }

  return napi_ok;
}

// Handle napi_value = napi_value__*, which we don't want to match
// but causes non-SFINAE compilation errors when used in std::is_base_of<>
// https://stackoverflow.com/a/8449204/20135
namespace detail {
template <typename T>
int is_complete_helper(int (*)[sizeof(T)]);

template <typename>
char is_complete_helper(...);

template <typename T>
constexpr bool is_complete_v = sizeof(is_complete_helper<T>(0)) != 1;

template <typename Base, typename Derived,
          typename = std::enable_if_t<is_complete_v<Derived>>>
constexpr bool is_complete_base_of_v = std::is_base_of_v<Base, Derived>;
}  // namespace detail

template <typename T, typename = std::enable_if_t<
                          detail::is_complete_base_of_v<NapiWrapped<T>, T>>>
napi_status napi_get_value(napi_env env, napi_value value, T** result) {
  auto status = NapiWrapped<T>::try_unwrap(env, value, result);
  if (status != napi_ok) {
    return napi_throw_last_error(env);
  }

  if (!*result) {
    napi_throw_type_error(env, nullptr, "Invalid native object type");
    return napi_pending_exception;
  }

  return napi_ok;
}

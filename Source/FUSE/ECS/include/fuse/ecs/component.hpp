#pragma once

#include <type_traits>

namespace fuse::ecs {

template <typename T, typename = void>
struct IsComponent : std::false_type {};

template <typename T>
struct IsComponent<T, std::void_t<decltype(T::component_name)>> : std::true_type {};

template <typename T>
inline constexpr bool IsComponentV = IsComponent<T>::value;

template <typename T>
inline constexpr const char* componentName() {
    return T::component_name;
}

} // namespace fuse::ecs

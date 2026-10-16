#pragma once

// Compatibility header for C++20 <concepts> on platforms where it's not fully supported
// (e.g., Android NDK 25 with API level 26)

#include <type_traits>

#if defined(__cpp_concepts) && __cpp_concepts >= 201907L && defined(__cpp_lib_concepts) && __cpp_lib_concepts >= 202002L
  // Full C++20 concepts support available
  #include <concepts>
#else
  // Provide fallback concepts for platforms without full C++20 concepts support
  namespace std {
    // floating_point concept: matches float, double, long double
    template<typename T>
    concept floating_point = std::is_floating_point_v<T>;

    // integral concept: matches integral types
    template<typename T>
    concept integral = std::is_integral_v<T>;

    // signed_integral concept: matches signed integral types
    template<typename T>
    concept signed_integral = std::is_integral_v<T> && std::is_signed_v<T>;

    // unsigned_integral concept: matches unsigned integral types
    template<typename T>
    concept unsigned_integral = std::is_integral_v<T> && !std::is_signed_v<T>;

    // same_as concept: matches if types are the same
    template<typename T, typename U>
    concept same_as = std::is_same_v<T, U> && std::is_same_v<U, T>;
  }
#endif

// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_SSH_KNOWN_HOSTS_ERROR_H_
#define ORBIT_SSH_KNOWN_HOSTS_ERROR_H_

#include <libssh2.h>

#include <outcome.hpp>
#include <system_error>

namespace OrbitSsh {

enum class KnownHostsError {
  kMismatch = LIBSSH2_KNOWNHOST_CHECK_MISMATCH,
  kNotFound = LIBSSH2_KNOWNHOST_CHECK_NOTFOUND,
  kFailure = LIBSSH2_KNOWNHOST_CHECK_FAILURE
};

struct KnownHostsErrorCategory : std::error_category {
  using std::error_category::error_category;

  const char* name() const noexcept override { return "libssh2_known_hosts"; }
  std::string message(int condition) const override;
};

inline const KnownHostsErrorCategory& GetKnownHostsErrorCategory() {
  static KnownHostsErrorCategory category{};
  return category;
}

inline std::error_code make_error_code(KnownHostsError e) {
  return std::error_code{static_cast<int>(e), GetKnownHostsErrorCategory()};
}

}  // namespace OrbitSsh

namespace std {
template <>
struct is_error_condition_enum<OrbitSsh::KnownHostsError> : std::true_type {};
}  // namespace std
#endif  // ORBIT_SSH_KNOWN_HOSTS_ERROR_H_

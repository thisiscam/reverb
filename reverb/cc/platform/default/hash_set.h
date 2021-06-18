// Copyright 2019 DeepMind Technologies Limited.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef REVERB_CC_PLATFORM_DEFAULT_HASH_SET_H_
#define REVERB_CC_PLATFORM_DEFAULT_HASH_SET_H_

// IWYU pragma: private
#include "absl/container/flat_hash_set.h"
#include "reverb/cc/platform/default/hash.h"

namespace deepmind {
namespace reverb {
namespace internal {

template <class T, class Hash = typename HashEq<T>::Hash,
          class Eq = typename HashEq<T>::Eq,
          class Allocator = typename absl::flat_hash_set<T>::allocator_type>
using flat_hash_set = absl::flat_hash_set<T, Hash, Eq, Allocator>;

}  // namespace internal
}  // namespace reverb
}  // namespace deepmind

#endif  // REVERB_CC_PLATFORM_DEFAULT_HASH_SET_H_

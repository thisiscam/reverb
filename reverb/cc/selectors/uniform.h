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

#ifndef REVERB_CC_SELECTORS_UNIFORM_H_
#define REVERB_CC_SELECTORS_UNIFORM_H_

#include <vector>

#include "absl/random/random.h"
#include "absl/status/status.h"
#include "reverb/cc/checkpointing/checkpoint.pb.h"
#include "reverb/cc/platform/hash_map.h"
#include "reverb/cc/selectors/interface.h"

namespace deepmind {
namespace reverb {

// Samples items uniformly and thus priority values have no effect. All
// operations take O(1) time. See ItemSelector for documentation of
// public methods.
class UniformSelector : public ItemSelector {
 public:
  absl::Status Delete(Key key) override;

  absl::Status Insert(Key key, double priority) override;

  absl::Status Update(Key key, double priority) override;

  KeyWithProbability Sample() override;

  void Clear() override;

  KeyDistributionOptions options() const override;

  std::string DebugString() const override;

 private:
  // All keys.
  std::vector<Key> keys_;

  // Maps a key to the index where this key can be found in `keys_.
  internal::flat_hash_map<Key, size_t> key_to_index_;

  // Used for sampling, not thread-safe.
  absl::BitGen bit_gen_;
};

}  // namespace reverb
}  // namespace deepmind

#endif  // REVERB_CC_SELECTORS_UNIFORM_H_

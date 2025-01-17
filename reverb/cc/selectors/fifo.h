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

#ifndef REVERB_CC_SELECTORS_FIFO_H_
#define REVERB_CC_SELECTORS_FIFO_H_

#include <list>

#include "reverb/cc/checkpointing/checkpoint.pb.h"
#include "reverb/cc/platform/hash_map.h"
#include "reverb/cc/selectors/interface.h"
#include "tensorflow/core/lib/core/status.h"

namespace deepmind {
namespace reverb {

// Fifo sampling. We ignore all priority values in the calls. Sample() always
// returns the key that was inserted first until this key is deleted. All
// operations take O(1) time. See ItemSelector for documentation
// about the methods.
class FifoSelector : public ItemSelector {
 public:
  tensorflow::Status Delete(Key key) override;

  // The priority is ignored.
  tensorflow::Status Insert(Key key, double priority) override;

  // This is a no-op but will return an error if the key does not exist.
  tensorflow::Status Update(Key key, double priority) override;

  KeyWithProbability Sample() override;

  void Clear() override;

  KeyDistributionOptions options() const override;

 private:
  std::list<Key> keys_;
  internal::flat_hash_map<Key, std::list<Key>::iterator> key_to_iterator_;
};

}  // namespace reverb
}  // namespace deepmind

#endif  // REVERB_CC_SELECTORS_FIFO_H_

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

#ifndef REVERB_CC_PLATFORM_TFRECORD_CHECKPOINTER_H_
#define REVERB_CC_PLATFORM_TFRECORD_CHECKPOINTER_H_

#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "reverb/cc/checkpointing/interface.h"
#include "reverb/cc/chunk_store.h"
#include "reverb/cc/table.h"

namespace deepmind {
namespace reverb {

// Generates and stores proto checkpoints of PriorityTables and ChunkStore data
// to a directory inside the top level `root_dir`.
//
// A set of `Table` constitutes the bases for a checkpoint. When `Save` is
// called state of each `Table` is encoded into a PriorityTableCheckpoint.
// The proto contains the state and initialization options of the table itself
// and all its dependencies (RateLimiter, KeyDistribution etc) but does not
// include the actual data. Instead a container with shared_ptr to every
// referenced ChunkStore::Chunk is attached which ensures that all data remains
// for the complete duration of the checkpointing operation.
//
// To avoid duplicating data, the union of the referenced chunks are
// deduplicated before being stored to disk. The stored checkpoint has the
// following format:
//
//   <root_dir>/
//     <timestamp of the checkpoint>/
//       tables.tfrecord
//       chunks.tfrecord
//       DONE
//
// DONE an empty file written once the checkpoint has been successfully written.
// If DONE does not exist then the checkpoint is in process of being written or
// the operation was unexpectedly interrupted and the data should be considered
// corrupt.
//
// The most recent checkpoint can therefore be inferred from the name of the
// directories within `root_dir`.
//
// If `group` is nonempty then the directory containing the checkpoint will be
// created with `group` as group.
//
// The optional class member `fallback_checkpoint_path` can be set to specify
// a checkpoint to be reloaded when no checkpoints can be found in `root_dir`.
// In practice, this enables use a checkpoint for previous experiment for
// initialization.
class TFRecordCheckpointer : public Checkpointer {
 public:
  explicit TFRecordCheckpointer(
      std::string root_dir, std::string group = "",
      absl::optional<std::string> fallback_checkpoint_path = absl::nullopt);

  // Save a new checkpoint for every table in `tables` in sub directory
  // inside `root_dir_`. If the call is successful, the ABSOLUTE path to the
  // newly created checkpoint directory is returned.
  //
  // If `root_path_` does not exist then `Save` attempts to recursively
  // create it before proceeding.
  //
  // If `fallback_checkpoint_path` is provided and iff no checkpoint is found in
  // `root_path_`, we attempt to reload the checkpoint with path
  // `fallback_checkpoint_path`. This way, we are effectively using the fallback
  // checkpoint as a way to initialise the service with a checkpoint generated
  // by another experiment.
  //
  // After a successful save, all but the `keep_latest` most recent checkpoints
  // are deleted.
  absl::Status Save(std::vector<Table*> tables, int keep_latest,
                    std::string* path) override;

  // Attempts to load a checkpoint stored within `root_dir_`.
  absl::Status Load(absl::string_view path, ChunkStore* chunk_store,
                    std::vector<std::shared_ptr<Table>>* tables) override;

  // Finds the most recent checkpoint within `root_dir_` and calls `Load`.
  absl::Status LoadLatest(ChunkStore* chunk_store,
                          std::vector<std::shared_ptr<Table>>* tables) override;

  // Attempts to load the fallback checkpoint. If no fallback_checkpoint_path
  // was set or if the no checkpoint found then `NotFoundError` is returned.
  absl::Status LoadFallbackCheckpoint(
      ChunkStore* chunk_store,
      std::vector<std::shared_ptr<Table>>* tables) override;

  // Returns a summary string description.
  std::string DebugString() const override;

  // TFRecordCheckpointer is neither copyable nor movable.
  TFRecordCheckpointer(const TFRecordCheckpointer&) = delete;
  TFRecordCheckpointer& operator=(const TFRecordCheckpointer&) = delete;

 private:
  const std::string root_dir_;
  const std::string group_;
  absl::optional<std::string> fallback_checkpoint_path_;
};

}  // namespace reverb
}  // namespace deepmind

#endif  // REVERB_CC_PLATFORM_TFRECORD_CHECKPOINTER_H_

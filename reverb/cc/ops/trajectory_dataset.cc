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

#include "absl/strings/match.h"
#include "reverb/cc/client.h"
#include "reverb/cc/errors.h"
#include "reverb/cc/platform/logging.h"
#include "reverb/cc/sampler.h"
#include "reverb/cc/support/tf_util.h"
#include "tensorflow/core/framework/common_shape_fns.h"
#include "tensorflow/core/framework/dataset.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/types.h"

namespace deepmind {
namespace reverb {
namespace {

using ::tensorflow::errors::Cancelled;
using ::tensorflow::errors::FailedPrecondition;
using ::tensorflow::errors::Unimplemented;

REGISTER_OP("ReverbTrajectoryDataset")
    .Input("server_address: string")
    .Input("table: string")
    .Attr("max_in_flight_samples_per_worker: int = 100")
    .Attr("num_workers_per_iterator: int = -1")
    .Attr("max_samples_per_stream: int = -1")
    .Attr("rate_limiter_timeout_ms: int = -1")
    .Attr("flexible_batch_size: int = -1")
    .Attr("dtypes: list(type) >= 1")
    .Attr("shapes: list(shape) >= 1")
    .Output("dataset: variant")
    .SetIsStateful()
    .SetShapeFn(tensorflow::shape_inference::ScalarShape)
    .Doc(R"doc(
Establishes and manages a connection to gRPC ReverbService at `server_address`
to stream samples from table `table`.

The connection is managed using a single instance of `Client` (see
../client.h) owned by the Dataset. From the shared `Client`, each iterator
maintains their own `Sampler` (see ../sampler.h), allowing for multiple
parallel streams using a single connection.

`dtypes` and `shapes` must match the type and shape of the trajectories
referenced by items in `table`.

`max_in_flight_samples_per_worker` (defaults to 100) is the maximum number of
 sampled item allowed to exist in flight (per iterator). See
`Sampler::Options::max_in_flight_samples_per_worker` for more details.

`num_workers_per_iterator` (defaults to -1, i.e auto selected) is the number of
worker threads to start per iterator. When the selected table uses a FIFO
sampler (i.e a queue) then exactly 1 worker must be used to avoid races causing
invalid ordering of items. For all other samplers, this value should be roughly
equal to the number of threads available on the CPU.

`max_samples_per_stream` (defaults to -1, i.e auto selected) is the maximum
number of samples to fetch from a stream before a new call is made. Keeping this
number low ensures that the data is fetched uniformly from all servers.

`rate_limiter_timeout_ms` (defaults to -1, i.e. never time out) is the number of
milliseconds an iterator should wait for new data from the sampler before timing
out. This can be useful, e.g., when the Reverb server receives data in
collection stages - and a dataset iterator should stop when no new data is
available for a while. If `rate_limiter_timeout_ms >= 0`, an iterator that waits
for data longer than this will close and mark the input sequence as finished.
Note that the timeout behavior depends on the Table's rate limiter. For example,
the table may contain data, but the rate limiter may pause sampling - and this
can cause a timeout to occur. Note also that when `num_workers_per_iterator >
1`, a timeout on any given worker will cause a timeout for the dataset.

`flexible_batch_size` [EXPERIMENTAL] (defaults to -1, i.e auto selected) is the
maximum number of items to sampled from `Table` with single call. Values > 1
enables `Table::SampleFlexibleBatch` to return more than one item (but no more
than `flexible_batch_size`) in a single call without releasing the table lock
iff the rate limiter allows it.
NOTE! It is unlikely that you need to tune this value yourself. The
auto selected value should almost always be preferred.
Larger `flexible_batch_size` values result a bias towards sampling over
inserts. In highly overloaded systems this results in higher sample QPS
and lower insert QPS compared to lower `flexible_batch_size` values.
)doc");

class ReverbTrajectoryDatasetOp : public tensorflow::data::DatasetOpKernel {
 public:
  explicit ReverbTrajectoryDatasetOp(tensorflow::OpKernelConstruction* ctx)
      : tensorflow::data::DatasetOpKernel(ctx) {
    OP_REQUIRES_OK(
        ctx, ctx->GetAttr("max_in_flight_samples_per_worker",
                          &sampler_options_.max_in_flight_samples_per_worker));
    OP_REQUIRES_OK(ctx, ctx->GetAttr("num_workers_per_iterator",
                                     &sampler_options_.num_workers));
    OP_REQUIRES_OK(ctx, ctx->GetAttr("max_samples_per_stream",
                                     &sampler_options_.max_samples_per_stream));
    OP_REQUIRES_OK(ctx, ctx->GetAttr("flexible_batch_size",
                                     &sampler_options_.flexible_batch_size));
    tensorflow::int64 rate_limiter_timeout_ms;
    OP_REQUIRES_OK(
        ctx, ctx->GetAttr("rate_limiter_timeout_ms", &rate_limiter_timeout_ms));
    OP_REQUIRES_OK(ctx, ctx->GetAttr("shapes", &shapes_));
    OP_REQUIRES_OK(ctx, ctx->GetAttr("dtypes", &dtypes_));

    sampler_options_.rate_limiter_timeout =
        Int64MillisToNonnegativeDuration(rate_limiter_timeout_ms);

    OP_REQUIRES_OK(ctx, ToTensorflowStatus(sampler_options_.Validate()));
  }

  void MakeDataset(tensorflow::OpKernelContext* ctx,
                   tensorflow::data::DatasetBase** output) override {
    tensorflow::tstring server_address;
    tensorflow::tstring table;
    OP_REQUIRES_OK(ctx,
                   tensorflow::data::ParseScalarArgument<tensorflow::tstring>(
                       ctx, "server_address", &server_address));
    OP_REQUIRES_OK(ctx,
                   tensorflow::data::ParseScalarArgument<tensorflow::tstring>(
                       ctx, "table", &table));

    *output = new Dataset(ctx, server_address, dtypes_, shapes_, table,
                          sampler_options_);
  }

 private:
  class Dataset : public tensorflow::data::DatasetBase {
   public:
    Dataset(tensorflow::OpKernelContext* ctx, std::string server_address,
            tensorflow::DataTypeVector dtypes,
            std::vector<tensorflow::PartialTensorShape> shapes,
            std::string table, const Sampler::Options& sampler_options)
        : tensorflow::data::DatasetBase(tensorflow::data::DatasetContext(ctx)),
          server_address_(std::move(server_address)),
          dtypes_(std::move(dtypes)),
          shapes_(std::move(shapes)),
          table_(std::move(table)),
          sampler_options_(sampler_options),
          client_(absl::make_unique<Client>(server_address_)) {}

    std::unique_ptr<tensorflow::data::IteratorBase> MakeIteratorInternal(
        const std::string& prefix) const override {
      return absl::make_unique<Iterator>(
          tensorflow::data::DatasetIterator<Dataset>::Params{
              this, absl::StrCat(prefix, "::ReverbDataset")},
          client_.get(), table_, sampler_options_, dtypes_, shapes_);
    }

    const tensorflow::DataTypeVector& output_dtypes() const override {
      return dtypes_;
    }

    const std::vector<tensorflow::PartialTensorShape>& output_shapes()
        const override {
      return shapes_;
    }

    std::string DebugString() const override {
      return "ReverbTrajectoryDatasetOp::Dataset";
    }

    tensorflow::Status CheckExternalState() const override {
      return FailedPrecondition(DebugString(), " depends on external state.");
    }

    tensorflow::Status InputDatasets(
        std::vector<const DatasetBase*>* inputs) const override {
      inputs->clear();
      return tensorflow::Status::OK();
    }

   protected:
    tensorflow::Status AsGraphDefInternal(
        tensorflow::data::SerializationContext* ctx, DatasetGraphDefBuilder* b,
        tensorflow::Node** output) const override {
      tensorflow::AttrValue max_in_flight_samples_per_worker_attr;
      tensorflow::AttrValue num_workers_attr;
      tensorflow::AttrValue max_samples_per_stream_attr;
      tensorflow::AttrValue rate_limiter_timeout_ms_attr;
      tensorflow::AttrValue flexible_batch_size_attr;
      tensorflow::AttrValue dtypes_attr;
      tensorflow::AttrValue shapes_attr;

      tensorflow::Node* server_address = nullptr;
      tensorflow::Node* table = nullptr;
      TF_RETURN_IF_ERROR(
          b->AddScalar<tensorflow::tstring>(server_address_, &server_address));
      TF_RETURN_IF_ERROR(b->AddScalar<tensorflow::tstring>(table_, &table));

      b->BuildAttrValue(sampler_options_.max_in_flight_samples_per_worker,
                        &max_in_flight_samples_per_worker_attr);
      b->BuildAttrValue(sampler_options_.num_workers, &num_workers_attr);
      b->BuildAttrValue(sampler_options_.max_samples_per_stream,
                        &max_samples_per_stream_attr);
      b->BuildAttrValue(
          static_cast<tensorflow::int64>(NonnegativeDurationToInt64Millis(
              sampler_options_.rate_limiter_timeout)),
          &rate_limiter_timeout_ms_attr);
      b->BuildAttrValue(sampler_options_.flexible_batch_size,
                        &flexible_batch_size_attr);
      b->BuildAttrValue(dtypes_, &dtypes_attr);
      b->BuildAttrValue(shapes_, &shapes_attr);

      TF_RETURN_IF_ERROR(b->AddDataset(
          this,
          /*inputs=*/{server_address, table},
          /*attrs=*/
          {
              {"max_in_flight_samples_per_worker",
               max_in_flight_samples_per_worker_attr},
              {"num_workers_per_iterator", num_workers_attr},
              {"max_samples_per_stream", max_samples_per_stream_attr},
              {"rate_limiter_timeout_ms", rate_limiter_timeout_ms_attr},
              {"flexible_batch_size", flexible_batch_size_attr},
              {"dtypes", dtypes_attr},
              {"shapes", shapes_attr},
          },
          output));

      return tensorflow::Status::OK();
    }

   private:
    class Iterator : public tensorflow::data::DatasetIterator<Dataset> {
     public:
      explicit Iterator(
          const Params& params, Client* client, const std::string& table,
          const Sampler::Options& sampler_options,
          const tensorflow::DataTypeVector& dtypes,
          const std::vector<tensorflow::PartialTensorShape>& shapes)
          : DatasetIterator<Dataset>(params),
            client_(client),
            table_(table),
            sampler_options_(sampler_options),
            dtypes_(dtypes),
            shapes_(shapes) {}

      tensorflow::Status Initialize(
          tensorflow::data::IteratorContext* ctx) override {
        constexpr auto kValidationTimeout = absl::Seconds(30);
        auto status =
            client_->NewSampler(table_, sampler_options_,
                                /*validation_dtypes=*/dtypes_, shapes_,
                                kValidationTimeout, &sampler_);
        if (absl::IsDeadlineExceeded(status)) {
          REVERB_LOG(REVERB_WARNING)
              << "Unable to validate shapes and dtypes of new sampler for '"
              << table_ << "' as server could not be reached in time ("
              << kValidationTimeout
              << "). We were thus unable to fetch signature from server. The "
                 "sampler will be constructed without validating the dtypes "
                 "and shapes.";
          // Ask for a NewSampler with negative validation_timeout Duration,
          // which causes it to skip the validation and return an OK status.
          return ToTensorflowStatus(client_->NewSampler(
              table_, sampler_options_,
              /*validation_timeout=*/-absl::InfiniteDuration(), &sampler_));
        }
        return ToTensorflowStatus(status);
      }

      tensorflow::Status GetNextInternal(
          tensorflow::data::IteratorContext* ctx,
          std::vector<tensorflow::Tensor>* out_tensors,
          bool* end_of_sequence) override {
        REVERB_CHECK(sampler_.get() != nullptr) << "Initialize was not called?";

        auto token = ctx->cancellation_manager()->get_cancellation_token();
        bool registered = ctx->cancellation_manager()->RegisterCallback(
            token, [&] { sampler_->Close(); });
        if (!registered) {
          sampler_->Close();
        }

        auto status =
            ToTensorflowStatus(sampler_->GetNextTrajectory(out_tensors));
        if (registered &&
            !ctx->cancellation_manager()->DeregisterCallback(token)) {
          return Cancelled("Iterator context was cancelled");
        }

        if (status.ok()) {
          *end_of_sequence = false;
          return status;
        } else if (sampler_options_.rate_limiter_timeout <
                       absl::InfiniteDuration() &&
                   errors::IsRateLimiterTimeout(FromTensorflowStatus(status))) {
          *end_of_sequence = true;
          return tensorflow::Status::OK();
        } else {
          return status;
        }
      }

     protected:
      tensorflow::Status SaveInternal(
          tensorflow::data::SerializationContext* ctx,
          tensorflow::data::IteratorStateWriter* writer) override {
        return Unimplemented("SaveInternal is currently not supported");
      }

      tensorflow::Status RestoreInternal(
          tensorflow::data::IteratorContext* ctx,
          tensorflow::data::IteratorStateReader* reader) override {
        return Unimplemented("RestoreInternal is currently not supported");
      }

     private:
      Client* client_;
      const std::string& table_;
      const Sampler::Options sampler_options_;
      const tensorflow::DataTypeVector& dtypes_;
      const std::vector<tensorflow::PartialTensorShape>& shapes_;
      std::unique_ptr<Sampler> sampler_;
    };  // Iterator.

    const std::string server_address_;
    const tensorflow::DataTypeVector dtypes_;
    const std::vector<tensorflow::PartialTensorShape> shapes_;
    const std::string table_;
    const Sampler::Options sampler_options_;
    std::unique_ptr<Client> client_;
  };  // Dataset.

  Sampler::Options sampler_options_;
  tensorflow::DataTypeVector dtypes_;
  std::vector<tensorflow::PartialTensorShape> shapes_;

  TF_DISALLOW_COPY_AND_ASSIGN(ReverbTrajectoryDatasetOp);
};

REGISTER_KERNEL_BUILDER(
    Name("ReverbTrajectoryDataset").Device(tensorflow::DEVICE_CPU),
    ReverbTrajectoryDatasetOp);

}  // namespace
}  // namespace reverb
}  // namespace deepmind

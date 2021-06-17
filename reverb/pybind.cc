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

#include <memory>
#include <string>

#include "numpy/arrayobject.h"
#include "absl/container/inlined_vector.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "pybind11/numpy.h"
#include "pybind11/pybind11.h"
#include "pybind11/stl.h"
#include "reverb/cc/checkpointing/interface.h"
#include "reverb/cc/chunker.h"
#include "reverb/cc/client.h"
#include "reverb/cc/platform/checkpointing.h"
#include "reverb/cc/platform/server.h"
#include "reverb/cc/rate_limiter.h"
#include "reverb/cc/sampler.h"
#include "reverb/cc/selectors/fifo.h"
#include "reverb/cc/selectors/heap.h"
#include "reverb/cc/selectors/interface.h"
#include "reverb/cc/selectors/lifo.h"
#include "reverb/cc/selectors/prioritized.h"
#include "reverb/cc/selectors/uniform.h"
#include "reverb/cc/support/tf_util.h"
#include "reverb/cc/table.h"
#include "reverb/cc/table_extensions/interface.h"
#include "reverb/cc/trajectory_writer.h"
#include "reverb/cc/writer.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/types.h"

namespace {

struct PyDecrefDeleter {
  void operator()(PyObject *p) const { Py_DECREF(p); }
};
using Safe_PyObjectPtr = std::unique_ptr<PyObject, PyDecrefDeleter>;
Safe_PyObjectPtr make_safe(PyObject *o) { return Safe_PyObjectPtr(o); }

// Converts non OK statuses to Python exceptions and throws. Does nothing for
// OK statuses.
inline void MaybeRaiseFromStatus(const absl::Status &status) {
  if (status.ok()) return;

  // TODO(b/152982733): Add tests that validates that casting behaviour is
  //   aligned with what tensorflow does.
  switch (status.code()) {
#define CODE_TO_PY_EXC(CODE, PY_EXC)                         \
  case CODE:                                                 \
    PyErr_SetString(PY_EXC, std::string(status.message()).data()); \
    break;

    CODE_TO_PY_EXC(absl::StatusCode::kInvalidArgument, PyExc_ValueError)
    CODE_TO_PY_EXC(absl::StatusCode::kResourceExhausted, PyExc_IndexError)
    CODE_TO_PY_EXC(absl::StatusCode::kUnimplemented, PyExc_NotImplementedError)
    CODE_TO_PY_EXC(absl::StatusCode::kInternal, PyExc_RuntimeError)

    // TODO(b/154927554): Map more status codes to Python exceptions.

#undef CODE_TO_PY_EXC

    default:
      PyErr_SetString(PyExc_RuntimeError, std::string(status.message()).data());
  }

  throw pybind11::error_already_set();
}

char const *NumpyTypeName(int numpy_type) {
  switch (numpy_type) {
#define TYPE_CASE(s) \
  case s:            \
    return #s

    TYPE_CASE(NPY_BOOL);
    TYPE_CASE(NPY_BYTE);
    TYPE_CASE(NPY_UBYTE);
    TYPE_CASE(NPY_SHORT);
    TYPE_CASE(NPY_USHORT);
    TYPE_CASE(NPY_INT);
    TYPE_CASE(NPY_UINT);
    TYPE_CASE(NPY_LONG);
    TYPE_CASE(NPY_ULONG);
    TYPE_CASE(NPY_LONGLONG);
    TYPE_CASE(NPY_ULONGLONG);
    TYPE_CASE(NPY_FLOAT);
    TYPE_CASE(NPY_DOUBLE);
    TYPE_CASE(NPY_LONGDOUBLE);
    TYPE_CASE(NPY_CFLOAT);
    TYPE_CASE(NPY_CDOUBLE);
    TYPE_CASE(NPY_CLONGDOUBLE);
    TYPE_CASE(NPY_OBJECT);
    TYPE_CASE(NPY_STRING);
    TYPE_CASE(NPY_UNICODE);
    TYPE_CASE(NPY_VOID);
    TYPE_CASE(NPY_DATETIME);
    TYPE_CASE(NPY_TIMEDELTA);
    TYPE_CASE(NPY_HALF);
    TYPE_CASE(NPY_NTYPES);
    TYPE_CASE(NPY_NOTYPE);
    TYPE_CASE(NPY_USERDEF);

    default:
      return "not a numpy type";
  }
}

void ImportNumpy() { import_array1(); }

tensorflow::Status PyObjectToString(PyObject *obj, const char **ptr,
                                    Py_ssize_t *len, PyObject **ptr_owner) {
  *ptr_owner = nullptr;
  if (PyBytes_Check(obj)) {
    char *buf;
    if (PyBytes_AsStringAndSize(obj, &buf, len) != 0) {
      return tensorflow::errors::Internal("Unable to get element as bytes.");
    }
    *ptr = buf;
  } else if (PyUnicode_Check(obj)) {
    *ptr = PyUnicode_AsUTF8AndSize(obj, len);
    if (*ptr == nullptr) {
      return tensorflow::errors::Internal("Unable to convert element to UTF-8");
    }
  } else {
    return tensorflow::errors::Internal("Unsupported object type ",
                                        obj->ob_type->tp_name);
  }

  return tensorflow::Status::OK();
}

// Iterate over the string array 'array', extract the ptr and len of each string
// element and call f(ptr, len).
template <typename F>
tensorflow::Status PyBytesArrayMap(PyArrayObject *array, F f) {
  auto iter = make_safe(PyArray_IterNew(reinterpret_cast<PyObject *>(array)));

  while (PyArray_ITER_NOTDONE(iter.get())) {
    auto item = make_safe(PyArray_GETITEM(
        array, static_cast<char *>(PyArray_ITER_DATA(iter.get()))));
    if (!item) {
      return tensorflow::errors::Internal(
          "Unable to get element from the feed - no item.");
    }
    Py_ssize_t len;
    const char *ptr;
    PyObject *ptr_owner = nullptr;
    TF_RETURN_IF_ERROR(PyObjectToString(item.get(), &ptr, &len, &ptr_owner));
    f(ptr, len);
    Py_XDECREF(ptr_owner);
    PyArray_ITER_NEXT(iter.get());
  }
  return tensorflow::Status::OK();
}

tensorflow::Status StringTensorToPyArray(const tensorflow::Tensor &tensor,
                                         PyArrayObject *dst) {
  DCHECK_EQ(tensor.dtype(), tensorflow::DT_STRING);

  auto iter = make_safe(PyArray_IterNew(reinterpret_cast<PyObject *>(dst)));

  const auto &flat_data = tensor.flat<tensorflow::tstring>().data();
  for (int i = 0; i < tensor.NumElements(); i++) {
    const auto &value = flat_data[i];
    auto py_string =
        make_safe(PyBytes_FromStringAndSize(value.c_str(), value.size()));
    if (py_string == nullptr) {
      return tensorflow::errors::Internal(
          "failed to create a python byte array when converting element #", i,
          " of a TF_STRING tensor to a numpy ndarray");
    }

    if (PyArray_SETITEM(dst, PyArray_ITER_DATA(iter.get()), py_string.get()) !=
        0) {
      return tensorflow::errors::Internal("Error settings element #", i,
                                          " in the numpy ndarray");
    }

    PyArray_ITER_NEXT(iter.get());
  }

  return tensorflow::Status::OK();
}

tensorflow::Status GetPyDescrFromDataType(tensorflow::DataType dtype,
                                          PyArray_Descr **out_descr) {
  switch (dtype) {
#define TF_TO_PY_ARRAY_TYPE_CASE(TF_DTYPE, PY_ARRAY_TYPE) \
  case TF_DTYPE:                                          \
    *out_descr = PyArray_DescrFromType(PY_ARRAY_TYPE);    \
    break;

    TF_TO_PY_ARRAY_TYPE_CASE(tensorflow::DT_HALF, NPY_FLOAT16)
    TF_TO_PY_ARRAY_TYPE_CASE(tensorflow::DT_FLOAT, NPY_FLOAT32)
    TF_TO_PY_ARRAY_TYPE_CASE(tensorflow::DT_DOUBLE, NPY_FLOAT64)
    TF_TO_PY_ARRAY_TYPE_CASE(tensorflow::DT_INT32, NPY_INT32)
    TF_TO_PY_ARRAY_TYPE_CASE(tensorflow::DT_UINT8, NPY_UINT8)
    TF_TO_PY_ARRAY_TYPE_CASE(tensorflow::DT_UINT16, NPY_UINT16)
    TF_TO_PY_ARRAY_TYPE_CASE(tensorflow::DT_UINT32, NPY_UINT32)
    TF_TO_PY_ARRAY_TYPE_CASE(tensorflow::DT_INT8, NPY_INT8)
    TF_TO_PY_ARRAY_TYPE_CASE(tensorflow::DT_INT16, NPY_INT16)
    TF_TO_PY_ARRAY_TYPE_CASE(tensorflow::DT_BOOL, NPY_BOOL)
    TF_TO_PY_ARRAY_TYPE_CASE(tensorflow::DT_COMPLEX64, NPY_COMPLEX64)
    TF_TO_PY_ARRAY_TYPE_CASE(tensorflow::DT_COMPLEX128, NPY_COMPLEX128)
    TF_TO_PY_ARRAY_TYPE_CASE(tensorflow::DT_STRING, NPY_OBJECT)
    TF_TO_PY_ARRAY_TYPE_CASE(tensorflow::DT_UINT64, NPY_UINT64)
    TF_TO_PY_ARRAY_TYPE_CASE(tensorflow::DT_INT64, NPY_INT64)

#undef TF_DTYPE_TO_PY_ARRAY_TYPE_CASE

    default:
      return tensorflow::errors::Internal(
          "Unsupported tf type: ", tensorflow::DataType_Name(dtype));
  }

  return tensorflow::Status::OK();
}

tensorflow::Status GetPyDescrFromTensor(const tensorflow::Tensor &tensor,
                                        PyArray_Descr **out_descr) {
  return GetPyDescrFromDataType(tensor.dtype(), out_descr);
}

tensorflow::Status GetTensorDtypeFromPyArray(
    PyArrayObject *array, tensorflow::DataType *out_tf_datatype) {
  int pyarray_type = PyArray_TYPE(array);
  switch (pyarray_type) {
#define NP_TO_TF_DTYPE_CASE(NP_DTYPE, TF_DTYPE) \
  case NP_DTYPE:                                \
    *out_tf_datatype = TF_DTYPE;                \
    break;

    NP_TO_TF_DTYPE_CASE(NPY_FLOAT16, tensorflow::DT_HALF)
    NP_TO_TF_DTYPE_CASE(NPY_FLOAT32, tensorflow::DT_FLOAT)
    NP_TO_TF_DTYPE_CASE(NPY_FLOAT64, tensorflow::DT_DOUBLE)

    NP_TO_TF_DTYPE_CASE(NPY_INT8, tensorflow::DT_INT8)
    NP_TO_TF_DTYPE_CASE(NPY_INT16, tensorflow::DT_INT16)
    NP_TO_TF_DTYPE_CASE(NPY_INT32, tensorflow::DT_INT32)
    NP_TO_TF_DTYPE_CASE(NPY_LONGLONG, tensorflow::DT_INT64)
    NP_TO_TF_DTYPE_CASE(NPY_INT64, tensorflow::DT_INT64)

    NP_TO_TF_DTYPE_CASE(NPY_UINT8, tensorflow::DT_UINT8)
    NP_TO_TF_DTYPE_CASE(NPY_UINT16, tensorflow::DT_UINT16)
    NP_TO_TF_DTYPE_CASE(NPY_UINT32, tensorflow::DT_UINT32)
    NP_TO_TF_DTYPE_CASE(NPY_ULONGLONG, tensorflow::DT_UINT64)
    NP_TO_TF_DTYPE_CASE(NPY_UINT64, tensorflow::DT_UINT64)

    NP_TO_TF_DTYPE_CASE(NPY_BOOL, tensorflow::DT_BOOL)

    NP_TO_TF_DTYPE_CASE(NPY_COMPLEX64, tensorflow::DT_COMPLEX64)
    NP_TO_TF_DTYPE_CASE(NPY_COMPLEX128, tensorflow::DT_COMPLEX128)

    NP_TO_TF_DTYPE_CASE(NPY_OBJECT, tensorflow::DT_STRING)
    NP_TO_TF_DTYPE_CASE(NPY_STRING, tensorflow::DT_STRING)
    NP_TO_TF_DTYPE_CASE(NPY_UNICODE, tensorflow::DT_STRING)

#undef NP_TO_TF_DTYPE_CASE

    case NPY_VOID:
      // TODO(b/154925774): Support struct and quantized types.
      return tensorflow::errors::Unimplemented(
          "Custom structs and quantized types are not supported");
    default:
      // TODO(b/154926401): Add support for bfloat16.
      // The bfloat16 type is defined in the internals of tf.
      if (pyarray_type == -1) {
        return tensorflow::errors::Unimplemented(
            "bfloat16 types are not yet supported");
      }

      return tensorflow::errors::Internal("Unsupported numpy type: ",
                                          NumpyTypeName(pyarray_type));
  }
  return tensorflow::Status::OK();
}

inline tensorflow::Status VerifyDtypeIsSupported(
    const tensorflow::DataType &dtype) {
  if (!tensorflow::DataTypeCanUseMemcpy(dtype) &&
      dtype != tensorflow::DT_STRING) {
    return tensorflow::errors::Unimplemented(
        "ndarrays that maps to tensors with dtype ",
        tensorflow::DataType_Name(dtype), " are not yet supported");
  }
  return tensorflow::Status::OK();
}

tensorflow::Status NdArrayToTensor(PyObject *ndarray,
                                   tensorflow::Tensor *out_tensor) {
  DCHECK(out_tensor != nullptr);
  auto array_safe = make_safe(PyArray_FromAny(
      /*op=*/ndarray,
      /*dtype=*/nullptr,
      /*min_depth=*/0,
      /*max_depth=*/0,
      /*requirements=*/NPY_ARRAY_CARRAY_RO,
      /*context=*/nullptr));
  if (!array_safe) {
    return tensorflow::errors::InvalidArgument(
        "Provided input could not be interpreted as an ndarray");
  }
  PyArrayObject *py_array = reinterpret_cast<PyArrayObject *>(array_safe.get());

  // Convert numpy dtype to TensorFlow dtype.
  tensorflow::DataType dtype;
  TF_RETURN_IF_ERROR(GetTensorDtypeFromPyArray(py_array, &dtype));
  TF_RETURN_IF_ERROR(VerifyDtypeIsSupported(dtype));

  absl::InlinedVector<tensorflow::int64, 4> dims(PyArray_NDIM(py_array));
  tensorflow::int64 nelems = 1;
  for (int i = 0; i < PyArray_NDIM(py_array); ++i) {
    dims[i] = PyArray_SHAPE(py_array)[i];
    nelems *= dims[i];
  }

  if (tensorflow::DataTypeCanUseMemcpy(dtype)) {
    *out_tensor = tensorflow::Tensor(dtype, tensorflow::TensorShape(dims));
    size_t size = PyArray_NBYTES(py_array);
    memcpy(out_tensor->data(), PyArray_DATA(py_array), size);
  } else if (dtype == tensorflow::DT_STRING) {
    *out_tensor = tensorflow::Tensor(dtype, tensorflow::TensorShape(dims));
    int i = 0;
    auto *out_t = out_tensor->flat<tensorflow::tstring>().data();
    TF_RETURN_IF_ERROR(
        PyBytesArrayMap(py_array, [out_t, &i](const char *ptr, Py_ssize_t len) {
          out_t[i++] = tensorflow::tstring(ptr, len);
        }));
  } else {
    return tensorflow::errors::Unimplemented("Unexpected dtype: ",
                                             tensorflow::DataTypeString(dtype));
  }

  return tensorflow::Status::OK();
}

tensorflow::Status TensorToNdArray(const tensorflow::Tensor &tensor,
                                   PyObject **out_ndarray) {
  TF_RETURN_IF_ERROR(VerifyDtypeIsSupported(tensor.dtype()));

  // Extract the numpy type and dimensions.
  PyArray_Descr *descr = nullptr;
  TF_RETURN_IF_ERROR(GetPyDescrFromTensor(tensor, &descr));

  absl::InlinedVector<npy_intp, 4> dims(tensor.dims());
  for (int i = 0; i < tensor.dims(); i++) {
    dims[i] = tensor.dim_size(i);
  }

  // Allocate an empty array of the desired shape and type.
  auto safe_out_ndarray =
      make_safe(PyArray_Empty(dims.size(), dims.data(), descr, 0));
  if (!safe_out_ndarray) {
    return tensorflow::errors::Internal("Could not allocate ndarray");
  }

  // Populate the ndarray with data from the tensor.
  PyArrayObject *py_array =
      reinterpret_cast<PyArrayObject *>(safe_out_ndarray.get());
  if (tensorflow::DataTypeCanUseMemcpy(tensor.dtype())) {
    memcpy(PyArray_DATA(py_array), tensor.data(), PyArray_NBYTES(py_array));
  } else if (tensor.dtype() == tensorflow::DT_STRING) {
    TF_RETURN_IF_ERROR(StringTensorToPyArray(tensor, py_array));
  } else {
    return tensorflow::errors::Unimplemented(
        "Unexpected tensor dtype: ",
        tensorflow::DataTypeString(tensor.dtype()));
  }

  *out_ndarray = safe_out_ndarray.release();
  return tensorflow::Status::OK();
}

// This wrapper exists for the sole purpose of allowing the weak_ptr to be
// handled in Python. Pybind supports shared_ptr and unique_ptr out of the box
// and although it is possible to implement our own `SmartPointer, using a
// minimal wrapper class like WeakCellRef is much simpler when the weak_ptr
// is only required for one class (in Python).
//
// See https://pybind11.readthedocs.io/en/stable/advanced/smart_ptrs.html for
// more information about smart pointers in pybind. To understand why a weak
// pointer is needed in the first place, please refer to the header and
// implementation of `CellRef`, `Chunker` and `TrajectoryWriter`.
class WeakCellRef {
 public:
  explicit WeakCellRef(std::weak_ptr<::deepmind::reverb::CellRef> ref)
      : ref_(std::move(ref)) {}

  std::weak_ptr<::deepmind::reverb::CellRef> ref() const { return ref_; }

  bool expired() const { return ref_.expired(); }

 private:
  std::weak_ptr<::deepmind::reverb::CellRef> ref_;
};

}  // namespace

namespace pybind11 {
namespace detail {

// Convert between absl::optional and python.
//
// pybind11 supports std::optional, and absl::optional is meant to be a
// drop-in replacement for std::optional, so we can just use the built in
// implementation.
//
// If we start getting errors due to this being defined in multiple places that
// likely means that pybind11 has included the cast itself and we can remove
// this implementation.
#ifndef ABSL_USES_STD_OPTIONAL
template <typename T>
struct type_caster<absl::optional<T>>
    : public optional_caster<absl::optional<T>> {};

template <>
struct type_caster<absl::nullopt_t> : public void_caster<absl::nullopt_t> {};
#endif

template <>
struct type_caster<tensorflow::Tensor> {
 public:
  PYBIND11_TYPE_CASTER(tensorflow::Tensor, _("tensorflow::Tensor"));

  bool load(handle handle, bool) {
    tensorflow::Status status = NdArrayToTensor(handle.ptr(), &value);

    if (!status.ok()) {
      std::string message = status.ToString();
      REVERB_LOG(REVERB_ERROR)
          << "Tensor can't be extracted from the source represented as "
             "ndarray: "
          << message;
      // When a conversion fails, PyErr is set. Returning from `load` with PyErr
      // set results in crashes so we clear the error here to make the Python
      // error slightly more readable.
      PyErr_Clear();
      return false;
    }
    return true;
  }

  static handle cast(const tensorflow::Tensor &src, return_value_policy,
                     handle) {
    PyObject *ret;
    tensorflow::Status status = TensorToNdArray(src, &ret);
    if (!status.ok()) {
      std::string message = status.ToString();
      PyErr_SetString(PyExc_ValueError, message.data());
      return nullptr;
    }
    return ret;
  }
};

// Raise an exception if a given status is not OK, otherwise return None.
template <>
struct type_caster<absl::Status> {
 public:
  PYBIND11_TYPE_CASTER(absl::Status, _("Status"));
  static handle cast(absl::Status status, return_value_policy, handle) {
    MaybeRaiseFromStatus(status);
    return none().inc_ref();
  }
};

}  // namespace detail
}  // namespace pybind11

// LINT.IfChange
namespace deepmind {
namespace reverb {
namespace {

namespace py = pybind11;

PYBIND11_MODULE(libpybind, m) {
  // Initialization code to use numpy types in the type casters.
  ImportNumpy();

  py::class_<ItemSelector, std::shared_ptr<ItemSelector>>(m, "ItemSelector")
      .def("__repr__", &ItemSelector::DebugString,
           py::call_guard<py::gil_scoped_release>());

  py::class_<PrioritizedSelector, ItemSelector,
             std::shared_ptr<PrioritizedSelector>>(m, "PrioritizedSelector")
      .def(py::init<double>(), py::arg("priority_exponent"));

  py::class_<FifoSelector, ItemSelector, std::shared_ptr<FifoSelector>>(
      m, "FifoSelector")
      .def(py::init());

  py::class_<LifoSelector, ItemSelector, std::shared_ptr<LifoSelector>>(
      m, "LifoSelector")
      .def(py::init());

  py::class_<UniformSelector, ItemSelector, std::shared_ptr<UniformSelector>>(
      m, "UniformSelector")
      .def(py::init());

  py::class_<HeapSelector, ItemSelector, std::shared_ptr<HeapSelector>>(
      m, "HeapSelector")
      .def(py::init<bool>(), py::arg("min_heap"));

  py::class_<TableExtension, std::shared_ptr<TableExtension>>(m,
                                                              "TableExtension")
      .def("__repr__", &TableExtension::DebugString,
           py::call_guard<py::gil_scoped_release>());

  py::class_<RateLimiter, std::shared_ptr<RateLimiter>>(m, "RateLimiter")
      .def(py::init<double, int, double, double>(),
           py::arg("samples_per_insert"), py::arg("min_size_to_sample"),
           py::arg("min_diff"), py::arg("max_diff"))
      .def("__repr__", &RateLimiter::DebugString,
           py::call_guard<py::gil_scoped_release>());

  py::class_<Table, std::shared_ptr<Table>>(m, "Table")
      .def(py::init(
               [](const std::string &name,
                  const std::shared_ptr<ItemSelector> &sampler,
                  const std::shared_ptr<ItemSelector> &remover, int max_size,
                  int max_times_sampled,
                  const std::shared_ptr<RateLimiter> &rate_limiter,
                  const std::vector<std::shared_ptr<TableExtension>>
                      &extensions,
                  const absl::optional<std::string> &serialized_signature =
                      absl::nullopt) -> Table * {
                 absl::optional<tensorflow::StructuredValue> signature =
                     absl::nullopt;
                 if (serialized_signature) {
                   signature.emplace();
                   if (!signature->ParseFromString(*serialized_signature)) {
                     MaybeRaiseFromStatus(
                         absl::InvalidArgumentError(absl::StrCat(
                             "Unable to deserialize StructuredValue from "
                             "serialized proto bytes: '",
                             *serialized_signature, "'")));
                     return nullptr;
                   }
                 }
                 return new Table(name, sampler, remover, max_size,
                                  max_times_sampled, rate_limiter, extensions,
                                  std::move(signature));
               }),
           py::arg("name"), py::arg("sampler"), py::arg("remover"),
           py::arg("max_size"), py::arg("max_times_sampled"),
           py::arg("rate_limiter"), py::arg("extensions"), py::arg("signature"))
      .def("name", &Table::name)
      .def("can_sample", &Table::CanSample,
           py::call_guard<py::gil_scoped_release>())
      .def("can_insert", &Table::CanInsert,
           py::call_guard<py::gil_scoped_release>())
      .def(
          "info",
          [](Table *table) -> py::bytes {
            // Return a serialized TableInfo proto bytes string.
            return py::bytes(table->info().SerializeAsString());
          },
          py::call_guard<py::gil_scoped_release>())
      .def("__repr__", &Table::DebugString,
           py::call_guard<py::gil_scoped_release>());

  py::class_<Writer>(m, "Writer")
      .def("Append", &Writer::Append, py::call_guard<py::gil_scoped_release>())
      .def("AppendSequence", &Writer::AppendSequence,
           py::call_guard<py::gil_scoped_release>())
      .def("CreateItem", &Writer::CreateItem,
           py::call_guard<py::gil_scoped_release>())
      .def(
          "Flush",
          [](Writer *writer) { MaybeRaiseFromStatus(writer->Flush()); },
          py::call_guard<py::gil_scoped_release>())
      .def("Close", &Writer::Close, py::call_guard<py::gil_scoped_release>())
      .def("__repr__", &Writer::DebugString,
           py::call_guard<py::gil_scoped_release>());

  py::class_<Sampler>(m, "Sampler")
      .def("GetNextTimestep",
           [](Sampler *sampler) {
             std::vector<tensorflow::Tensor> sample;
             bool end_of_sequence;
             absl::Status status;

             // Release the GIL only when waiting for the call to complete. If
             // the GIL is not held when `MaybeRaiseFromStatus` is called it can
             // result in segfaults as the Python exception is populated with
             // details from the status.
             {
               py::gil_scoped_release g;
               status = sampler->GetNextTimestep(&sample, &end_of_sequence);
             }

             MaybeRaiseFromStatus(status);
             return std::make_pair(std::move(sample), end_of_sequence);
           })
      .def("GetNextTrajectory",
           [](Sampler *sampler) {
             absl::Status status;
             std::vector<tensorflow::Tensor> sample;

             // Release the GIL only when waiting for the call to complete. If
             // the GIL is not held when `MaybeRaiseFromStatus` is called it can
             // result in segfaults as the Python exception is populated with
             // details from the status.
             {
               py::gil_scoped_release g;
               status = sampler->GetNextTrajectory(&sample);
             }

             MaybeRaiseFromStatus(status);
             return sample;
           })
      .def("Close", &Sampler::Close, py::call_guard<py::gil_scoped_release>());

  py::class_<Client>(m, "Client")
      .def(py::init<std::string>(), py::arg("server_name"))
      .def(
          "NewWriter",
          [](Client *client, int chunk_length, int max_timesteps,
             bool delta_encoded, absl::optional<int> max_in_flight_items) {
            std::unique_ptr<Writer> writer;
            MaybeRaiseFromStatus(
                client->NewWriter(chunk_length, max_timesteps, delta_encoded,
                                  std::move(max_in_flight_items), &writer));
            return writer;
          },
          py::call_guard<py::gil_scoped_release>(), py::arg("chunk_length"),
          py::arg("max_timesteps"), py::arg("delta_encoded") = false,
          py::arg("max_in_flight_items"))
      .def(
          "NewSampler",
          [](Client *client, const std::string &table, int64_t max_samples,
             size_t buffer_size) {
            std::unique_ptr<Sampler> sampler;
            Sampler::Options options;
            options.max_samples = max_samples;
            options.max_in_flight_samples_per_worker = buffer_size;
            MaybeRaiseFromStatus(client->NewSamplerWithoutSignatureCheck(
                table, options, &sampler));
            return sampler;
          },
          py::call_guard<py::gil_scoped_release>())
      .def("NewTrajectoryWriter",
           [](Client *client, std::shared_ptr<ChunkerOptions> chunker_options,
              absl::optional<int> get_signature_timeout_ms) {
             std::unique_ptr<TrajectoryWriter> writer;

             TrajectoryWriter::Options options;
             options.chunker_options = std::move(chunker_options);

             // Release the GIL only when waiting for the call to complete. If
             // the GIL is not held when `MaybeRaiseFromStatus` is called it can
             // result in segfaults as the Python exception is populated with
             // details from the status.
             absl::Status status;
             if (get_signature_timeout_ms.has_value()) {
               py::gil_scoped_release g;

               status = client->NewTrajectoryWriter(
                   options,
                   absl::Milliseconds(get_signature_timeout_ms.value()),
                   &writer);
             } else {
               status = client->NewTrajectoryWriter(options, &writer);
             }
             MaybeRaiseFromStatus(status);

             return writer.release();
           })
      .def(
          "MutatePriorities",
          [](Client *client, const std::string &table,
             const std::vector<std::pair<uint64_t, double>> &updates,
             const std::vector<uint64_t> &deletes) {
            std::vector<KeyWithPriority> update_protos;
            for (const auto &update : updates) {
              update_protos.emplace_back();
              update_protos.back().set_key(update.first);
              update_protos.back().set_priority(update.second);
            }
            return client->MutatePriorities(table, update_protos, deletes);
          },
          py::call_guard<py::gil_scoped_release>())
      .def("Reset", &Client::Reset, py::call_guard<py::gil_scoped_release>())
      .def("ServerInfo",
           [](Client *client, int timeout_sec) {
             // Wait indefinetely for server to startup when timeout not
             // provided.
             auto timeout = timeout_sec > 0 ? absl::Seconds(timeout_sec)
                                            : absl::InfiniteDuration();

             struct Client::ServerInfo info;

             // Release the GIL only when waiting for the call to complete. If
             // the GIL is not held when `MaybeRaiseFromStatus` is called it can
             // result in segfaults as the Python exception is populated with
             // details from the status.
             absl::Status status;
             {
               py::gil_scoped_release g;
               status = client->ServerInfo(timeout, &info);
             }
             MaybeRaiseFromStatus(status);

             // Return a serialized ServerInfo proto bytes string.
             std::vector<py::bytes> serialized_table_info;
             serialized_table_info.reserve(info.table_info.size());
             for (const auto &table_info : info.table_info) {
               serialized_table_info.push_back(
                   py::bytes(table_info.SerializeAsString()));
             }
             return serialized_table_info;
           })
      .def("Checkpoint", [](Client *client) {
        std::string path;
        absl::Status status;
        {
          py::gil_scoped_release g;
          status = client->Checkpoint(&path);
        }
        MaybeRaiseFromStatus(status);
        return path;
      });

  py::class_<Checkpointer, std::shared_ptr<Checkpointer>>(m, "Checkpointer")
      .def("__repr__", &Checkpointer::DebugString,
           py::call_guard<py::gil_scoped_release>());

  m.def(
      "create_default_checkpointer",
      [](const std::string &name, const std::string &group,
         absl::optional<std::string> fallback_checkpoint_path) {
        auto checkpointer = CreateDefaultCheckpointer(
            name, group, std::move(fallback_checkpoint_path));
        return std::shared_ptr<Checkpointer>(checkpointer.release());
      },
      py::call_guard<py::gil_scoped_release>());

  py::class_<Server, std::shared_ptr<Server>>(m, "Server")
      .def(
          py::init([](std::vector<std::shared_ptr<Table>> priority_tables,
                      int port,
                      std::shared_ptr<Checkpointer> checkpointer = nullptr) {
            std::unique_ptr<Server> server;
            MaybeRaiseFromStatus(StartServer(std::move(priority_tables), port,
                                             std::move(checkpointer), &server));
            return server.release();
          }),
          py::arg("priority_tables"), py::arg("port"),
          py::arg("checkpointer") = nullptr)
      .def("Stop", &Server::Stop, py::call_guard<py::gil_scoped_release>())
      .def("Wait", &Server::Wait, py::call_guard<py::gil_scoped_release>())
      .def("InProcessClient", &Server::InProcessClient,
           py::call_guard<py::gil_scoped_release>())
      .def("__repr__", &Server::DebugString,
           py::call_guard<py::gil_scoped_release>());

  py::class_<WeakCellRef, std::shared_ptr<WeakCellRef>>(m, "WeakCellRef")
      .def_property_readonly("expired", &WeakCellRef::expired)
      .def("numpy",
           [](WeakCellRef *ref) -> tensorflow::Tensor {
             tensorflow::Tensor tensor;

             auto sp = ref->ref().lock();
             if (!sp) {
               MaybeRaiseFromStatus(absl::FailedPreconditionError(
                   "Cannot access data from expired WeakCellRef"));
               return tensor;
             }

             absl::Status status;
             {
               py::gil_scoped_release g;
               status = sp->GetData(&tensor);
             }
             MaybeRaiseFromStatus(status);

             return tensor;
           })
      .def_property_readonly(
          "shape",
          [](WeakCellRef *ref) -> std::vector<absl::optional<int>> {
            std::vector<absl::optional<int>> out_shape;

            auto sp = ref->ref().lock();
            if (!sp) {
              MaybeRaiseFromStatus(absl::FailedPreconditionError(
                  "Cannot access data from expired WeakCellRef"));
              return out_shape;
            }

            absl::Status status;
            {
              py::gil_scoped_release g;
              internal::TensorSpec spec;
              status = sp->GetSpec(&spec);
              out_shape.reserve(spec.shape.dims());
              for (auto dim : spec.shape.dim_sizes()) {
                // Replace -1 with absl::nullopt because the Python API uses
                // None instead of -1 to represent unknown dimensions.
                out_shape.push_back(dim == -1 ? absl::nullopt
                                              : absl::make_optional(dim));
              }
            }
            MaybeRaiseFromStatus(status);

            return out_shape;
          })
      .def_property_readonly(
          "dtype", [](WeakCellRef *ref) -> py::dtype {
            auto sp = ref->ref().lock();
            if (!sp) {
              MaybeRaiseFromStatus(absl::FailedPreconditionError(
                  "Cannot access data from expired WeakCellRef"));
            }

            absl::Status status;
            py::dtype dtype;
            {
              py::gil_scoped_release g;
              internal::TensorSpec spec;
              status = sp->GetSpec(&spec);

              if (status.ok()) {
                PyArray_Descr *descr = nullptr;
                status = FromTensorflowStatus(
                    GetPyDescrFromDataType(spec.dtype, &descr));
                if (status.ok()) {
                  dtype = py::reinterpret_steal<py::dtype>(
                      reinterpret_cast<PyObject *>(descr));
                }
              }
            }
            MaybeRaiseFromStatus(status);
            return dtype;
          });

  py::class_<ChunkerOptions, std::shared_ptr<ChunkerOptions>>(m,
                                                              "ChunkerOptions");

  py::class_<ConstantChunkerOptions, ChunkerOptions,
             std::shared_ptr<ConstantChunkerOptions>>(m,
                                                      "ConstantChunkerOptions")
      .def(py::init<int, int>(), py::arg("max_chunk_length"),
           py::arg("num_keep_alive_refs"))
      .def("__eq__", [](ConstantChunkerOptions *self,
                        std::shared_ptr<ConstantChunkerOptions> other) {
        return self->GetMaxChunkLength() == other->GetMaxChunkLength() &&
               self->GetNumKeepAliveRefs() == other->GetNumKeepAliveRefs();
      });

  py::class_<AutoTunedChunkerOptions, ChunkerOptions,
             std::shared_ptr<AutoTunedChunkerOptions>>(
      m, "AutoTunedChunkerOptions")
      .def(py::init<int, double>(), py::arg("num_keep_alive_refs"),
           py::arg("throughput_weight"))
      .def("__eq__", [](AutoTunedChunkerOptions *self,
                        std::shared_ptr<AutoTunedChunkerOptions> other) {
        return self->GetNumKeepAliveRefs() == other->GetNumKeepAliveRefs();
      });

  py::class_<TrajectoryWriter, std::shared_ptr<TrajectoryWriter>>(
      m, "TrajectoryWriter")
      .def(
          "Append",
          [](TrajectoryWriter *writer,
             std::vector<absl::optional<tensorflow::Tensor>> data) {
            std::vector<absl::optional<std::weak_ptr<CellRef>>> refs;
            MaybeRaiseFromStatus(writer->Append(std::move(data), &refs));

            std::vector<absl::optional<std::shared_ptr<WeakCellRef>>> weak_refs(
                refs.size());
            for (int i = 0; i < refs.size(); i++) {
              if (refs[i].has_value()) {
                weak_refs[i] =
                    std::make_shared<WeakCellRef>(std::move(refs[i].value()));
              } else {
                weak_refs[i] = absl::nullopt;
              }
            }

            return weak_refs;
          })
      .def(
          "AppendPartial",
          [](TrajectoryWriter *writer,
             std::vector<absl::optional<tensorflow::Tensor>> data) {
            std::vector<absl::optional<std::weak_ptr<CellRef>>> refs;
            MaybeRaiseFromStatus(writer->AppendPartial(std::move(data), &refs));

            std::vector<absl::optional<std::shared_ptr<WeakCellRef>>> weak_refs(
                refs.size());
            for (int i = 0; i < refs.size(); i++) {
              if (refs[i].has_value()) {
                weak_refs[i] =
                    std::make_shared<WeakCellRef>(std::move(refs[i].value()));
              } else {
                weak_refs[i] = absl::nullopt;
              }
            }

            return weak_refs;
          })
      .def(
          "CreateItem",
          [](TrajectoryWriter *writer, const std::string &table,
             double priority,
             std::vector<std::vector<std::shared_ptr<WeakCellRef>>>
                 py_trajectory,
             std::vector<bool> squeeze_column) {
            if (py_trajectory.size() != squeeze_column.size()) {
              MaybeRaiseFromStatus(absl::InternalError(
                  "Length of py_trajectory and squeeze_column did not match."));
              return;
            }

            std::vector<TrajectoryColumn> trajectory;
            trajectory.reserve(py_trajectory.size());
            for (int i = 0; i < py_trajectory.size(); i++) {
              auto &py_column = py_trajectory[i];
              std::vector<std::weak_ptr<CellRef>> column;
              column.reserve(py_column.size());
              for (auto &weak_ref : py_column) {
                column.push_back(weak_ref->ref());
              }
              trajectory.push_back(
                  TrajectoryColumn(std::move(column), squeeze_column[i]));
            }
            MaybeRaiseFromStatus(
                writer->CreateItem(table, priority, trajectory));
          })
      .def("Flush",
           [](TrajectoryWriter *writer, int ignore_last_num_items,
              int timeout_ms) {
             absl::Status status;
             auto timeout = timeout_ms > 0 ? absl::Milliseconds(timeout_ms)
                                           : absl::InfiniteDuration();
             {
               py::gil_scoped_release g;
               status = writer->Flush(ignore_last_num_items, timeout);
             }
             MaybeRaiseFromStatus(status);
           })
      .def("EndEpisode",
           [](TrajectoryWriter *writer, bool clear_buffers,
              absl::optional<int> timeout_ms) {
             absl::Status status;
             {
               py::gil_scoped_release g;
               status = writer->EndEpisode(
                   clear_buffers, timeout_ms.has_value()
                                      ? absl::Milliseconds(timeout_ms.value())
                                      : absl::InfiniteDuration());
             }
             MaybeRaiseFromStatus(status);
           })
      .def("Close", &TrajectoryWriter::Close,
           py::call_guard<py::gil_scoped_release>())
      .def("ConfigureChunker", &TrajectoryWriter::ConfigureChunker,
           py::call_guard<py::gil_scoped_release>());
}

}  // namespace
}  // namespace reverb
}  // namespace deepmind
// LINT.ThenChange(pybind.pyi)

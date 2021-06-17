workspace(name = "reverb")

# To change to a version of protoc compatible with tensorflow:
#  1. Convert the required header version to a version string, e.g.:
#     3011004 => "3.11.4"
#  2. Calculate the sha256 of the binary:
#     PROTOC_VERSION="3.11.4"
#     curl -L "https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOC_VERSION}/protoc-${PROTOC_VERSION}-linux-x86_64.zip" | sha256sum
#  3. Update the two variables below.
#
# Alternatively, run bazel with the environment var "REVERB_PROTOC_VERSION"
# set to override PROTOC_VERSION.
#
# *WARNING* If using the REVERB_PROTOC_VERSION environment variable, sha256
# checking is disabled.  Use at your own risk.
PROTOC_VERSION = "3.9.0"
PROTOC_SHA256 = "15e395b648a1a6dda8fd66868824a396e9d3e89bc2c8648e3b9ab9801bea5d55"

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

http_archive(
    name = "com_google_googletest",
    sha256 = "ff7a82736e158c077e76188232eac77913a15dac0b22508c390ab3f88e6d6d86",
    strip_prefix = "googletest-b6cd405286ed8635ece71c72f118e659f4ade3fb",
    urls = [
        "https://storage.googleapis.com/mirror.tensorflow.org/github.com/google/googletest/archive/b6cd405286ed8635ece71c72f118e659f4ade3fb.zip",
        "https://github.com/google/googletest/archive/b6cd405286ed8635ece71c72f118e659f4ade3fb.zip",
    ],
)

http_archive(
    name = "com_google_absl",
    sha256 = "35f22ef5cb286f09954b7cc4c85b5a3f6221c9d4df6b8c4a1e9d399555b366ee",  # SHARED_ABSL_SHA
    strip_prefix = "abseil-cpp-997aaf3a28308eba1b9156aa35ab7bca9688e9f6",
    patch_cmds=[
        """curl -sSL https://github.com/derekmauro/abseil-cpp/commit/2da3ef0d639481b638da2d626c7ea75984e85f05.diff | patch -p1"""
    ],
    urls = [
        "https://storage.googleapis.com/mirror.tensorflow.org/github.com/abseil/abseil-cpp/archive/997aaf3a28308eba1b9156aa35ab7bca9688e9f6.tar.gz",
        "https://github.com/abseil/abseil-cpp/archive/997aaf3a28308eba1b9156aa35ab7bca9688e9f6.tar.gz",
    ],
)


# http_archive(
#     name = "com_google_absl",
#     sha256 = "f368a8476f4e2e0eccf8a7318b98dafbe30b2600f4e3cf52636e5eb145aba06a",  # SHARED_ABSL_SHA
#     strip_prefix = "abseil-cpp-df3ea785d8c30a9503321a3d35ee7d35808f190d",
#     urls = [
#         "https://storage.googleapis.com/mirror.tensorflow.org/github.com/abseil/abseil-cpp/archive/df3ea785d8c30a9503321a3d35ee7d35808f190d.tar.gz",
#         "https://github.com/abseil/abseil-cpp/archive/df3ea785d8c30a9503321a3d35ee7d35808f190d.tar.gz",
#     ],
# )


## Begin GRPC related deps
http_archive(
    name = "com_github_grpc_grpc",
    patch_cmds = [
        """sed -i.bak 's/"python",/"python3",/g' third_party/py/python_configure.bzl""",
        """sed -i.bak 's/PYTHONHASHSEED=0/PYTHONHASHSEED=0 python3/g' bazel/cython_library.bzl""",
    ],
    sha256 = "45b5956d3c807fbf9045d946864d83013543a2474420cffaa984022428242271",
    strip_prefix = "grpc-ce05bf557ced2d311bad8ee520f9f8088f715bd8",
    urls = [
        "https://github.com/grpc/grpc/archive/ce05bf557ced2d311bad8ee520f9f8088f715bd8.tar.gz",
    ],
)


load("@com_github_grpc_grpc//bazel:grpc_deps.bzl", "grpc_deps")

grpc_deps()

load("@com_github_grpc_grpc//bazel:grpc_extra_deps.bzl", "grpc_extra_deps")

grpc_extra_deps()


load("@upb//bazel:workspace_deps.bzl", "upb_deps")

upb_deps()

load(
    "@build_bazel_rules_apple//apple:repositories.bzl",
    "apple_rules_dependencies",
)

apple_rules_dependencies()

load(
    "@build_bazel_apple_support//lib:repositories.bzl",
    "apple_support_dependencies",
)

apple_support_dependencies()
## End GRPC related deps

load(
    "//reverb/cc/platform/default:repo.bzl",
    "cc_tf_configure",
    "reverb_protoc_deps",
    "reverb_python_deps",
)

cc_tf_configure()

reverb_python_deps()

reverb_protoc_deps(version = PROTOC_VERSION, sha256 = PROTOC_SHA256)

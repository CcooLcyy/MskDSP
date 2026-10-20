set(PLATFORM_PROTO_ROOT "${CMAKE_SOURCE_DIR}/platform-proto")
set(PLATFORM_PROTO_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${PLATFORM_PROTO_GENERATED}")
set(PLATFORM_GRPC_PLUGIN "$<TARGET_FILE:gRPC::grpc_cpp_plugin>")
if(CMAKE_CROSSCOMPILING)
  set(PLATFORM_GRPC_PLUGIN "${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}/tools/grpc/grpc_cpp_plugin")
endif()
set(PLATFORM_PROTO_OUTPUTS
  "${PLATFORM_PROTO_GENERATED}/platform/v1/remote.pb.cc"
  "${PLATFORM_PROTO_GENERATED}/platform/v1/remote.pb.h"
  "${PLATFORM_PROTO_GENERATED}/platform/v1/remote.grpc.pb.cc"
  "${PLATFORM_PROTO_GENERATED}/platform/v1/remote.grpc.pb.h")
add_custom_command(OUTPUT ${PLATFORM_PROTO_OUTPUTS}
  COMMAND protobuf::protoc
    "--proto_path=${PLATFORM_PROTO_ROOT}"
    "--cpp_out=${PLATFORM_PROTO_GENERATED}"
    "--grpc_out=${PLATFORM_PROTO_GENERATED}"
    "--plugin=protoc-gen-grpc=${PLATFORM_GRPC_PLUGIN}"
    "${PLATFORM_PROTO_ROOT}/platform/v1/remote.proto"
  DEPENDS "${PLATFORM_PROTO_ROOT}/platform/v1/remote.proto" protobuf::protoc
  VERBATIM)
add_library(platformRemoteProto STATIC ${PLATFORM_PROTO_OUTPUTS})
set_target_properties(platformRemoteProto PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(platformRemoteProto PUBLIC "${PLATFORM_PROTO_GENERATED}")
target_link_libraries(platformRemoteProto PUBLIC gRPC::grpc++ protobuf::libprotobuf)

add_library(platformAgent STATIC PlatformAgent.cc PlatformAgentConfig.cc PlatformTunnel.cc)
target_include_directories(platformAgent PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(platformAgent PUBLIC platformRemoteProto moduleManager dspProto)

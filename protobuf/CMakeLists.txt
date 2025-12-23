file(GLOB PROTO_FILE_SRCS *.proto)

add_library(dspProto SHARED ${PROTO_FILE_SRCS})
target_link_libraries(dspProto PUBLIC 
  gRPC::grpc++
  protobuf::libprotobuf
)

protobuf_generate(TARGET dspProto)

protobuf_generate(
  TARGET dspProto
  LANGUAGE grpc
  PLUGIN protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
  PLUGIN_OPTIONS generate_mock_code=true
  GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
)

target_include_directories(dspProto PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)
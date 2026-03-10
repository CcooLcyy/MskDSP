set(DEVICE_SIM_TARGET dlt645_pcd_sim)

add_executable(${DEVICE_SIM_TARGET}
  src/main.cc
)

target_link_libraries(${DEVICE_SIM_TARGET}
  PRIVATE
    Boost::json
    util
)

set_target_properties(${DEVICE_SIM_TARGET}
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${MSKDSP_TOOLS_OUTPUT_DIR}"
)

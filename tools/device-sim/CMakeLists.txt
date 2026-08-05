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

set(IEC61850_MMS_SIM_TARGET iec61850_mms_sim)

add_executable(${IEC61850_MMS_SIM_TARGET}
  src/iec61850_mms_sim.cc
)

target_include_directories(${IEC61850_MMS_SIM_TARGET}
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src/IEC61850/include
    ${MSKDSP_COMMON_INCLUDE_DIR}
)

target_link_libraries(${IEC61850_MMS_SIM_TARGET}
  PRIVATE
    IEC61850
)

set_target_properties(${IEC61850_MMS_SIM_TARGET}
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${MSKDSP_TOOLS_OUTPUT_DIR}"
    BUILD_WITH_INSTALL_RPATH OFF
    BUILD_RPATH "${MSKDSP_PACKAGE_DIR}/module;${MSKDSP_PACKAGE_DIR}/lib"
    INSTALL_RPATH "$ORIGIN/../module;$ORIGIN/../lib"
)

set(IEC61850_MMS_SMOKE_TARGET iec61850_mms_worker_smoke)

add_executable(${IEC61850_MMS_SMOKE_TARGET}
  src/iec61850_mms_worker_smoke.cc
)

target_include_directories(${IEC61850_MMS_SMOKE_TARGET}
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src/IEC61850/include
    ${MSKDSP_COMMON_INCLUDE_DIR}
)

target_link_libraries(${IEC61850_MMS_SMOKE_TARGET}
  PRIVATE
    IEC61850
)

set_target_properties(${IEC61850_MMS_SMOKE_TARGET}
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${MSKDSP_TOOLS_OUTPUT_DIR}"
    BUILD_WITH_INSTALL_RPATH OFF
    BUILD_RPATH "${MSKDSP_PACKAGE_DIR}/module;${MSKDSP_PACKAGE_DIR}/lib"
    INSTALL_RPATH "$ORIGIN/../module;$ORIGIN/../lib"
)

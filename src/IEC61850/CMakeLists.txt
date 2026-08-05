include(cmake/LibInfo.cmake)

configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/LibInfo.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/include/${LIB_NAME}LibInfo.h
)

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} MODULE_SRCS)
add_library(${LIB_NAME} SHARED ${MODULE_SRCS})
add_dependencies(${LIB_NAME} dspProto)

target_sources(${LIB_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850CommandExecutorService.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850SvState.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850GooseRuntime.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850SvRuntime.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850RawEthernet.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850MmsTransport.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850MmsChannelPolicy.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850MmsExchange.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850MmsBer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850MmsPdu.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850MmsService.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850MmsJournal.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850MmsDynamicDataSet.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850MmsFile.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850MmsControl.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850MmsReport.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850MmsIsoSession.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850MmsRcb.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850MmsWorker.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850MmsSettingGroup.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850MmsServer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850RawProtocolStack.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850RealtimeSignalProcessor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850ProtectionEngine.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850ProtectionActionDispatcher.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850GoosePublisher.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850SvPublisher.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850SvMath.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850SvMathEngine.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IEC61850ThreadRuntimePolicy.h
)

target_include_directories(${LIB_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${MSKDSP_COMMON_INCLUDE_DIR}
)
target_link_libraries(${LIB_NAME} PUBLIC
    moduleManager
    mskdspConfigStore
    pugixml::pugixml
)
set_target_properties(${LIB_NAME} PROPERTIES
    VERSION ${VERSION}
    SOVERSION ${VERSION_MAJOR}
)

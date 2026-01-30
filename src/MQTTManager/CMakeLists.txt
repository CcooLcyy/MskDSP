include(cmake/LibInfo.cmake)

find_package(PahoMqttCpp CONFIG REQUIRED)

set(MQTT_PAHO_TARGET "")
if(TARGET PahoMqttCpp::paho-mqttpp3)
  set(MQTT_PAHO_TARGET PahoMqttCpp::paho-mqttpp3)
elseif(TARGET pahomqttcpp::paho-mqttpp3)
  set(MQTT_PAHO_TARGET pahomqttcpp::paho-mqttpp3)
elseif(TARGET paho-mqttpp3::paho-mqttpp3)
  set(MQTT_PAHO_TARGET paho-mqttpp3::paho-mqttpp3)
else()
  message(FATAL_ERROR "未找到 Paho MQTT C++ 库目标")
endif()

configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/LibInfo.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/include/${LIB_NAME}LibInfo.h
)

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} MODULE_SRCS)
add_library(${LIB_NAME} SHARED ${MODULE_SRCS})

target_include_directories(${LIB_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(${LIB_NAME} PUBLIC
    moduleManager
    ${MQTT_PAHO_TARGET}
)
set_target_properties(${LIB_NAME} PROPERTIES
    VERSION ${VERSION}
    SOVERSION ${VERSION_MAJOR}
)

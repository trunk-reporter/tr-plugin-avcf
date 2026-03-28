find_package(eclipse-paho-mqtt-c REQUIRED)
find_package(PahoMqttCpp REQUIRED)
set(PahoMqttC_LIBRARIES eclipse-paho-mqtt-c::paho-mqtt3as)
set(PahoMqttCpp_LIBRARIES PahoMqttCpp::paho-mqttpp3)

add_library(mqtt_avcf
MODULE
  mqtt_avcf.cc
)

include_directories(
  ${CMAKE_BINARY_DIR}/../
)

target_link_libraries(mqtt_avcf
  ${PahoMqttC_LIBRARIES}
  ${PahoMqttCpp_LIBRARIES}
  trunk_recorder_library
  ssl
  crypto
  ${Boost_LIBRARIES}
  ${GNURADIO_PMT_LIBRARIES}
  ${GNURADIO_RUNTIME_LIBRARIES}
  ${GNURADIO_FILTER_LIBRARIES}
  ${GNURADIO_DIGITAL_LIBRARIES}
  ${GNURADIO_ANALOG_LIBRARIES}
  ${GNURADIO_AUDIO_LIBRARIES}
  ${GNURADIO_UHD_LIBRARIES}
  ${UHD_LIBRARIES}
  ${GNURADIO_BLOCKS_LIBRARIES}
  ${GNURADIO_OSMOSDR_LIBRARIES}
  ${LIBOP25_REPEATER_LIBRARIES}
  gnuradio-op25_repeater
)

if(NOT Gnuradio_VERSION VERSION_LESS "3.8")
  target_link_libraries(mqtt_avcf
    gnuradio::gnuradio-analog
    gnuradio::gnuradio-blocks
    gnuradio::gnuradio-digital
    gnuradio::gnuradio-filter
    gnuradio::gnuradio-pmt
  )
endif()

install(TARGETS mqtt_avcf LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/trunk-recorder)

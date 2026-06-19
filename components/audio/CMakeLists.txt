set(srcs audio_transport.c pkt.c jitter.c)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${srcs}
                           INCLUDE_DIRS "."
                           REQUIRES pthread
                           PRIV_REQUIRES lwip)
else()
    add_library(audio STATIC ${srcs})
    target_include_directories(audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(audio PUBLIC pthread)
endif()

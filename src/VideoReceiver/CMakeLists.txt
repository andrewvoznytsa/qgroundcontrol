set(EXTRA_LIBRARIES)

if (GST_FOUND)
    set(EXTRA_LIBRARIES ${GST_LIBRARIES})
endif()

add_library(VideoReceiver
    gstqgc.c
    gstqgcvideosinkbin.c
    GStreamer.cc
    VideoReceiver.cc
    VideoReceiver.h
)

target_link_libraries(VideoReceiver
    PUBLIC
        qgc
        Qt5::Multimedia
        Qt5::OpenGL
        ${EXTRA_LIBRARIES}
)

target_include_directories(VideoReceiver INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_library(VideoReceiver
    gstqgc.c
    gstqgcvideosinkbin.c
    GStreamer.cc
    VideoReceiver.cc
    VideoReceiver.h
)

target_link_libraries(VideoReceiver
    PUBLIC
        Qt5::Multimedia
        Qt5::OpenGL
        Qt5::Quick
        qmlglsink
        ${GST_LIBRARIES}
)

target_include_directories(VideoReceiver INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

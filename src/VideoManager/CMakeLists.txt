set(EXTRA_LIBRARIES)

if (GST_FOUND)
    set(EXTRA_LIBRARIES VideoReceiver)
endif()

add_library(VideoManager
    GLVideoItemStub.cc
    GLVideoItemStub.h
    SubtitleWriter.cc
    SubtitleWriter.h
    VideoManager.cc
    VideoManager.h
)

target_link_libraries(VideoManager
    PUBLIC
        qgc
        Qt5::Multimedia
        Qt5::OpenGL
        ${EXTRA_LIBRARIES}
)

target_include_directories(VideoManager INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

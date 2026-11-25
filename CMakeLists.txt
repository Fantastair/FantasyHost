cmake_minimum_required(VERSION 3.21)
project(FantasyHost VERSION 0.1.0 LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets SerialPort)
qt_standard_project_setup()
option(FANTASYHOST_OPENGL "Build the optional OpenGL canvas viewport" ON)
if(FANTASYHOST_OPENGL)
    find_package(Qt6OpenGLWidgets 6.5 QUIET)
endif()

qt_add_executable(FantasyHost WIN32 MACOSX_BUNDLE
    src/main.cpp
    src/graph.cpp src/graph.h
    src/modules.cpp src/modules.h
    src/textdisplay.cpp src/textdisplay.h
    src/runner.cpp src/runner.h
    src/scope.cpp src/scope.h
    src/spectrum.cpp src/spectrum.h
    src/waveform.cpp src/waveform.h
    src/theme.cpp src/theme.h
    src/canvas.cpp src/canvas.h
    src/window.cpp src/window.h
)
target_compile_features(FantasyHost PRIVATE cxx_std_17)
target_compile_definitions(FantasyHost PRIVATE QT_NO_KEYWORDS)
target_link_libraries(FantasyHost PRIVATE Qt6::Widgets Qt6::SerialPort)
if(WIN32)
    target_link_libraries(FantasyHost PRIVATE ole32 shell32 uuid)
endif()
if(FANTASYHOST_OPENGL AND TARGET Qt6::OpenGLWidgets)
    target_link_libraries(FantasyHost PRIVATE Qt6::OpenGLWidgets)
    target_compile_definitions(FantasyHost PRIVATE FANTASYHOST_OPENGL)
endif()
if(MSVC)
    target_compile_options(FantasyHost PRIVATE /utf-8)
endif()

install(TARGETS FantasyHost BUNDLE DESTINATION . RUNTIME DESTINATION bin)
if(WIN32 AND Qt6_VERSION VERSION_GREATER_EQUAL 6.8)
    set(deploy_options DEPLOY_TOOL_OPTIONS
        --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw
        --skip-plugin-types generic,imageformats,networkinformation,tls
    )
endif()
qt_generate_deploy_app_script(
    TARGET FantasyHost
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
    NO_TRANSLATIONS
    ${deploy_options}
)
install(SCRIPT ${deploy_script})

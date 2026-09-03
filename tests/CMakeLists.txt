# Each test is a standalone executable linking gvtcore. Agents: add yours here.
file(GLOB TEST_SRCS CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
foreach(t ${TEST_SRCS})
    get_filename_component(name ${t} NAME_WE)
    add_executable(${name} ${t})
    target_link_libraries(${name} PRIVATE gvtcore)
    if(name STREQUAL "test_fit_button")
        target_link_libraries(${name} PRIVATE Qt6::Widgets)
    elseif(name STREQUAL "test_ui_layout")
        target_sources(${name} PRIVATE
            ${CMAKE_SOURCE_DIR}/src/ui/MainWindow.cpp
            ${CMAKE_SOURCE_DIR}/src/ui/DeckWidget.cpp
            ${CMAKE_SOURCE_DIR}/src/ui/DetailWaveformView.cpp
            ${CMAKE_SOURCE_DIR}/src/ui/MixerWidget.cpp
            ${CMAKE_SOURCE_DIR}/src/ui/LibraryWidget.cpp
            ${CMAKE_SOURCE_DIR}/src/ui/TransitionPanel.cpp
            ${CMAKE_SOURCE_DIR}/src/ui/Flx4TutorialWidget.cpp)
        target_link_libraries(${name} PRIVATE Qt6::Widgets)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endforeach()

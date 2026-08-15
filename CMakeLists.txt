cmake_minimum_required(VERSION 3.24)
project(gravitino VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)

list(APPEND CMAKE_PREFIX_PATH /opt/homebrew/opt/qt /opt/homebrew)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent)

find_library(RTMIDI_LIB rtmidi PATHS /opt/homebrew/opt/rtmidi/lib REQUIRED)
find_path(RTMIDI_INCLUDE rtmidi/RtMidi.h PATHS /opt/homebrew/opt/rtmidi/include REQUIRED)
find_library(TAGLIB_LIB tag PATHS /opt/homebrew/opt/taglib/lib REQUIRED)
find_path(TAGLIB_INCLUDE taglib/fileref.h PATHS /opt/homebrew/opt/taglib/include REQUIRED)

# miniaudio compiled once as C to keep C++ TUs fast
add_library(miniaudio STATIC third_party/miniaudio_impl.c)
target_include_directories(miniaudio PUBLIC third_party)

add_library(gvtcore STATIC
    src/control/ControlBus.cpp
    src/audio/AudioEngine.cpp
    src/audio/Deck.cpp
    src/audio/Eq.cpp
    src/analysis/TrackData.cpp
    src/analysis/BeatAnalyzer.cpp
    src/library/TrackLibrary.cpp
    src/library/TransitionStore.cpp
    src/transitions/GvtFormat.cpp
    src/transitions/TransitionRecorder.cpp
    src/transitions/TransitionPlayer.cpp
    src/midi/MidiEngine.cpp
    src/midi/Flx4Mapping.cpp
)
target_include_directories(gvtcore PUBLIC src ${RTMIDI_INCLUDE} ${TAGLIB_INCLUDE})
target_link_libraries(gvtcore PUBLIC
    Qt6::Core Qt6::Gui Qt6::Concurrent miniaudio ${RTMIDI_LIB} ${TAGLIB_LIB}
    "-framework CoreAudio" "-framework CoreMIDI" "-framework CoreFoundation")

add_executable(gravitino MACOSX_BUNDLE
    src/app/main.cpp
    src/app/SelfTest.cpp
    src/ui/MainWindow.cpp
    src/ui/DeckWidget.cpp
    src/ui/DetailWaveformView.cpp
    src/ui/MixerWidget.cpp
    src/ui/LibraryWidget.cpp
    src/ui/TransitionPanel.cpp
)
set_target_properties(gravitino PROPERTIES MACOSX_BUNDLE FALSE) # plain binary for dev
target_link_libraries(gravitino PRIVATE gvtcore Qt6::Widgets)

enable_testing()
add_subdirectory(tests)

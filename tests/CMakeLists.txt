# Each test is a standalone executable linking gvtcore. Agents: add yours here.
file(GLOB TEST_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
foreach(t ${TEST_SRCS})
    get_filename_component(name ${t} NAME_WE)
    add_executable(${name} ${t})
    target_link_libraries(${name} PRIVATE gvtcore)
    add_test(NAME ${name} COMMAND ${name})
endforeach()

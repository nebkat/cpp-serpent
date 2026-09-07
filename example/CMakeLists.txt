# The examples are built and run as tests, so they cannot quietly stop compiling.

set(EXAMPLES
    quickstart
    reading_without_copying
    reflection
    transcoding)

foreach(EXAMPLE IN LISTS EXAMPLES)
    add_executable(${EXAMPLE}_example "${EXAMPLE}.cpp")
    target_link_libraries(${EXAMPLE}_example PRIVATE serpent)
    target_compile_options(${EXAMPLE}_example PRIVATE -Wall -Wextra)
    add_test(NAME example_${EXAMPLE} COMMAND ${EXAMPLE}_example)
endforeach()

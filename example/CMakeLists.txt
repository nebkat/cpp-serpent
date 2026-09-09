# The examples are built and run as tests, so they cannot quietly stop compiling.

set(EXAMPLES
    reading_without_copying
    transcoding)

# These define a type, which means annotating it.
if(SERPENT_HAS_REFLECTION_SUPPORT)
    list(APPEND EXAMPLES quickstart reflection splicing)
endif()

foreach(EXAMPLE IN LISTS EXAMPLES)
    add_executable(${EXAMPLE}_example "${EXAMPLE}.cpp")
    target_link_libraries(${EXAMPLE}_example PRIVATE serpent)
    target_compile_options(${EXAMPLE}_example PRIVATE -Wall -Wextra)
    add_test(NAME example_${EXAMPLE} COMMAND ${EXAMPLE}_example)
endforeach()

set(EXAMPLE_NAMES fork snake stopit hello_jmpbuf gdb_backtrace)

foreach(name IN LISTS EXAMPLE_NAMES)
    add_executable(example_${name} ${name}.c)
    target_compile_options(example_${name} PRIVATE -g)
    set_target_properties(example_${name} PROPERTIES
        OUTPUT_NAME ${name}
        EXCLUDE_FROM_ALL TRUE
    )
endforeach()

add_custom_target(examples)
foreach(name IN LISTS EXAMPLE_NAMES)
    add_dependencies(examples example_${name})
endforeach()

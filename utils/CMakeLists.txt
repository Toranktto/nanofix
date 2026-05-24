if(NANOFIX_BUILD_FIXSPEC_GEN)
    find_package(pugixml REQUIRED)
    add_executable(fixspec-gen
        fixspec-gen/fixspec-gen.cpp)
    target_link_libraries(fixspec-gen PRIVATE pugixml::pugixml)
    install(TARGETS fixspec-gen RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(NANOFIX_BUILD)
    add_executable(fixprint fixprint/fixprint.cpp)
    target_link_libraries(fixprint PRIVATE nanofix)

    add_executable(fixgen fixgen/fixgen.cpp)
    target_link_libraries(fixgen PRIVATE nanofix)

    install(TARGETS fixprint fixgen
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

set(SOURCES
    main.cpp
    mainwindow.cpp
    mainwindow.h
)

set(RESOURCES
    ../resources/blitzview.qrc
)

add_executable(blitzview ${SOURCES} ${RESOURCES})

target_link_libraries(blitzview PRIVATE Qt6::Widgets)

# Installation
install(TARGETS blitzview
    RUNTIME DESTINATION bin
)

qt_add_executable(collabedit
    main.cpp
    MainWindow.cpp
    MainWindow.h
    Document.cpp
    Document.h
    CollabPane.cpp
    CollabPane.h
)
target_link_libraries(collabedit PRIVATE Qt6::Widgets CollabText::CollabText)
target_include_directories(collabedit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/collabtext/src)

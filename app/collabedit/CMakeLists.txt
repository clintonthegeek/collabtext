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

if(COLLABTEXT_BUILD_TESTS)
    qt_add_executable(tst_editor_recovery
        ../../libs/collabtext/tests/tst_editor_recovery.cpp
        Document.cpp
        Document.h
        CollabPane.cpp
        CollabPane.h)
    target_link_libraries(tst_editor_recovery PRIVATE
        CollabText::CollabText Qt6::Test)
    target_include_directories(tst_editor_recovery PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/collabtext/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/collabtext/tests)
    add_test(NAME tst_editor_recovery COMMAND tst_editor_recovery)
endif()

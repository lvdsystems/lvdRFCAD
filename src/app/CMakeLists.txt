add_executable(lvdrfcad WIN32 main.cpp lvdrfcad.rc)
target_link_libraries(lvdrfcad PRIVATE lvdgui)

if(WIN32 AND LVD_DEPLOY_QT)
    find_program(LVD_WINDEPLOYQT windeployqt HINTS "${LVD_QT_BIN_DIR}" REQUIRED)
    add_custom_command(TARGET lvdrfcad POST_BUILD
        COMMAND "${LVD_WINDEPLOYQT}" --no-translations --no-compiler-runtime
                --no-system-d3d-compiler --no-opengl-sw "$<TARGET_FILE:lvdrfcad>"
        COMMENT "Deploying the Qt runtime next to lvdrfcad"
        VERBATIM)
endif()

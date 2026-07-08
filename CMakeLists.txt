cmake_minimum_required(VERSION 3.16)

project(aoa_h264server C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(aoa_start_lib aoa_start.c)

add_executable(aoa_h264server aoa_h264server.c)
target_link_libraries(aoa_h264server PRIVATE aoa_start_lib)

add_executable(aoa_start_tool aoa_start_tool.c)
target_link_libraries(aoa_start_tool PRIVATE aoa_start_lib)

if(WIN32)
    target_compile_definitions(aoa_h264server PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(aoa_h264server PRIVATE ws2_32)

    set(LIBUSB_ROOT "" CACHE PATH "Path to a libusb Windows package, such as a vcpkg installed tree")

    find_path(LIBUSB_INCLUDE_DIR
        NAMES libusb.h libusb-1.0/libusb.h
        HINTS "${LIBUSB_ROOT}/include" "${LIBUSB_ROOT}/include/libusb-1.0"
    )

    find_library(LIBUSB_LIBRARY
        NAMES libusb-1.0 usb-1.0 libusb
        HINTS
            "${LIBUSB_ROOT}/lib"
            "${LIBUSB_ROOT}/lib/manual-link"
            "${LIBUSB_ROOT}/MinGW64/static"
            "${LIBUSB_ROOT}/VS2022/MS64/dll"
            "${LIBUSB_ROOT}/VS2025/MS64/dll"
    )

    find_file(LIBUSB_RUNTIME_DLL
        NAMES libusb-1.0.dll
        HINTS
            "${LIBUSB_ROOT}/MinGW64/dll"
            "${LIBUSB_ROOT}/VS2022/MS64/dll"
            "${LIBUSB_ROOT}/VS2025/MS64/dll"
    )

    if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
        message(FATAL_ERROR "libusb was not found. Configure with -DLIBUSB_ROOT=C:/path/to/libusb/package")
    endif()

    target_include_directories(aoa_start_lib PUBLIC "${LIBUSB_INCLUDE_DIR}")
    target_link_libraries(aoa_start_lib PUBLIC "${LIBUSB_LIBRARY}")

    if(LIBUSB_RUNTIME_DLL)
        add_custom_command(TARGET aoa_h264server POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LIBUSB_RUNTIME_DLL}"
                "$<TARGET_FILE_DIR:aoa_h264server>"
        )
        add_custom_command(TARGET aoa_start_tool POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LIBUSB_RUNTIME_DLL}"
                "$<TARGET_FILE_DIR:aoa_start_tool>"
        )
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

    target_include_directories(aoa_start_lib PUBLIC ${LIBUSB_INCLUDE_DIRS})
    target_link_directories(aoa_start_lib PUBLIC ${LIBUSB_LIBRARY_DIRS})
    target_link_libraries(aoa_start_lib PUBLIC ${LIBUSB_LIBRARIES})
    target_link_libraries(aoa_h264server PRIVATE pthread)
endif()

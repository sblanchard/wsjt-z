find_package (Qt5Test 5 REQUIRED)

#
# Compiler options
#
set (CMAKE_CXX_STANDARD 11)
add_compile_options ("$<$<COMPILE_LANGUAGE:Fortran>:-Wall;-Wno-conversion;-fno-second-underscore;-fno-f2c>")
add_compile_options ("$<$<AND:$<COMPILE_LANGUAGE:Fortran>,$<CONFIG:Debug>>:-fbounds-check>")
add_compile_options ("$<$<AND:$<COMPILE_LANGUAGE:Fortran>,$<NOT:$<CONFIG:Debug>>>:-funroll-all-loops>")
add_compile_options ("$<$<AND:$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>,$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>,$<C_COMPILER_ID:AppleClang>>>:-Wall;-Wextra>")
add_compile_options ("$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:-Wno-pragmas>")
add_compile_options ("$<$<AND:$<OR:$<C_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:GNU>>,$<NOT:$<CONFIG:Debug>>>:-fdata-sections;-ffunction-sections>")
if (${OPENMP_FOUND} OR APPLE)
  add_compile_options ("$<$<AND:$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>,$<C_COMPILER_ID:GNU>>:${OpenMP_C_FLAGS}>")
endif ()

# Tell CMake to run moc when necessary
set (CMAKE_AUTOMOC ON)
include_directories (${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_SOURCE_DIR})

add_executable (test_qt_helpers test_qt_helpers.cpp)
target_link_libraries (test_qt_helpers wsjt_qt Qt5::Test)
add_test (test_qt_helpers test_qt_helpers)

add_executable (test_flex_socket_compat test_flex_socket_compat.cpp)
target_link_libraries (test_flex_socket_compat wsjt_qt Qt5::Test)
add_test (test_flex_socket_compat test_flex_socket_compat)

add_executable (test_flex_vita_receiver test_flex_vita_receiver.cpp)
target_link_libraries (test_flex_vita_receiver wsjt_qt Qt5::Test Qt5::Network)
add_test (test_flex_vita_receiver test_flex_vita_receiver)

add_executable (test_native_flex_factory test_native_flex_factory.cpp)
target_include_directories (test_native_flex_factory PRIVATE ${CMAKE_BINARY_DIR})
target_link_libraries (test_native_flex_factory wsjt_qt wsjt_cxx Qt5::Test)
if ((NOT ${OPENMP_FOUND}) OR APPLE)
  target_link_libraries (test_native_flex_factory wsjt_fort)
else ()
  target_link_libraries (test_native_flex_factory wsjt_fort_omp)
endif ()
add_test (test_native_flex_factory test_native_flex_factory)

add_executable (test_native_flex_selection test_native_flex_selection.cpp)
target_link_libraries (test_native_flex_selection wsjt_qt Qt5::Test)
add_test (test_native_flex_selection test_native_flex_selection)

add_executable (test_tx_rf_power_plumbing test_tx_rf_power_plumbing.cpp)
target_include_directories (test_tx_rf_power_plumbing PRIVATE ${CMAKE_BINARY_DIR})
target_link_libraries (test_tx_rf_power_plumbing wsjt_qt Qt5::Test)
add_test (test_tx_rf_power_plumbing test_tx_rf_power_plumbing)

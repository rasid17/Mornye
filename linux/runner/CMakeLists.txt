cmake_minimum_required(VERSION 3.13)
project(runner LANGUAGES CXX)

# Define the application target. To change its name, change BINARY_NAME in the
# top-level CMakeLists.txt, not the value here, or `flutter run` will no longer
# work.
#
# Any new source files that you add to the application should be added here.
add_executable(${BINARY_NAME}
  "main.cc"
  "my_application.cc"
  "nvidia_graphics_workaround.cc"
  "${FLUTTER_MANAGED_DIR}/generated_plugin_registrant.cc"
)

apply_standard_settings(${BINARY_NAME})

add_definitions(-DAPPLICATION_ID="${APPLICATION_ID}")

# libflutter_linux_gtk.so exports its own unwinder (_Unwind_RaiseException
# and friends). Linked ahead of it, libgcc_s comes first in the symbol lookup
# order, so a C++ exception or a LuaJIT error in libmpv unwinds with libgcc's
# unwinder; libflutter's cannot walk LuaJIT's frames and crashed the app.
target_link_libraries(${BINARY_NAME} PRIVATE "-Wl,--no-as-needed" gcc_s)
target_link_libraries(${BINARY_NAME} PRIVATE flutter)
target_link_libraries(${BINARY_NAME} PRIVATE PkgConfig::GTK)
target_link_libraries(${BINARY_NAME} PRIVATE ${CMAKE_DL_LIBS})

target_include_directories(${BINARY_NAME} PRIVATE "${CMAKE_SOURCE_DIR}")

# An out-of-tree consumer of the installed FUSE UI package (gate fuse_ui_install_consumer): it sees only the install
# tree (find_package(FuseUI)), never the source tree.
cmake_minimum_required(VERSION 3.21)
project(fuse_ui_consumer LANGUAGES C CXX)
find_package(FuseUI 0.1 REQUIRED)
add_executable(fuse_ui_consumer main.cpp)
target_link_libraries(fuse_ui_consumer PRIVATE FuseUI::widgets FuseUI::doc FuseUI::app)
message(STATUS "FuseUI targets: ${FuseUI_TARGETS}")

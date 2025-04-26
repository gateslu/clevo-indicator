cmake_minimum_required(VERSION 3.10)

# 项目名称和版本
project(clevo-indicator VERSION 1.0.0)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 找到 GTK 包
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

# 添加 libayatana-appindicator3-dev 依赖
pkg_check_modules(APPINDICATOR REQUIRED IMPORTED_TARGET ayatana-appindicator3-0.1)

find_package(nlohmann_json 3.2.0 REQUIRED)

# 添加可执行文件
add_executable(clevo-indicator src/clevo-indicator.cpp)

# 设置包含目录
target_include_directories(clevo-indicator PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/single_include
        ${GTK3_INCLUDE_DIRS}
        ${APPINDICATOR_INCLUDE_DIRS}
)

# 链接库
target_link_libraries(clevo-indicator PRIVATE PkgConfig::GTK3 PkgConfig::APPINDICATOR)
target_link_libraries(clevo-indicator PRIVATE nlohmann_json::nlohmann_json)

# 安装规则
install(TARGETS clevo-indicator DESTINATION bin)

# 测试支持（如果需要）
enable_testing()
add_test(NAME clevo-indicator_test COMMAND clevo-indicator)
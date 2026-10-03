cmake_minimum_required(VERSION 3.20)

project(iotd VERSION 0.2.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 全局编译设置
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)          # 用 C++17：std::optional / std::variant / std::string_view
set(CMAKE_CXX_STANDARD_REQUIRED ON) # 编译器不支持 C++17 就直接报错，而不是偷偷降级
set(CMAKE_CXX_EXTENSIONS OFF)       # 不用 GNU 扩展，保证代码可移植

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)     # 不指定时默认 Debug，方便用 gdb 调试
endif()

# 打开全部警告：新手最容易犯的错（未初始化变量、类型转换）编译器都会提醒你
if(MSVC)
    add_compile_options(/W4 /utf-8)   # /utf-8 防止中文注释和字符串乱码
else()
    add_compile_options(-Wall -Wextra)
    # 静态链接 GCC 自己的运行库（libgcc / libstdc++）。
    # 好处：exe 少依赖两个 DLL，拷到别的机器上更容易跑起来。
    # 这也是很多商业软件交付时的标准做法。
    add_link_options(-static-libgcc -static-libstdc++)
endif()

# 让 CMake 在 build/ 目录里生成 compile_commands.json。
# 这个文件记录"每个 .cpp 到底是拿哪条完整命令编译的"（编译器路径、-I 头文件
# 目录、-std=c++17、所有宏）。VSCode 的 C/C++ 插件靠它做代码补全和跳转，
# 于是 CMakeLists.txt 成了编译设置的唯一来源，不用再手写 c_cpp_properties.json。
# 它只影响生成的文件，不影响编译出的 exe。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# 查找 libmosquitto（第二期新增）
#
# 【为什么要手写这段？】
# 理想情况下一句 find_package(mosquitto) 就够了，
# 但 MSYS2 的 mosquitto 包【没有提供 CMake config 文件】
# （实测 C:\msys64\ucrt64\lib\cmake\ 下没有 mosquitto 目录），
# 所以 CMake 找不到它，必须用 pkg-config 或 find_library 手动定位。
#
# 这两种写法你都要认识，因为真实项目里会不断遇到"不带 CMake 支持"的库：
#   pkg-config   —— 老牌手段，.pc 文件里写着编译参数（Linux/MSYS2 常见）
#   find_library —— 最原始的手段，直接去 lib 目录里找 .dll.a / .lib / .so
# ---------------------------------------------------------------------------
find_package(PkgConfig QUIET)

set(MOSQUITTO_TARGET "")

if(PKG_CONFIG_FOUND)
    pkg_check_modules(MOSQUITTO IMPORTED_TARGET libmosquitto)
    if(TARGET PkgConfig::MOSQUITTO)
        set(MOSQUITTO_TARGET PkgConfig::MOSQUITTO)
        message(STATUS "找到 libmosquitto（经由 pkg-config）版本=${MOSQUITTO_VERSION}")
    endif()
endif()

if(NOT MOSQUITTO_TARGET)
    # 兜底：手动找头文件和库文件
    find_path(MOSQUITTO_INCLUDE_DIR NAMES mosquitto.h)
    find_library(MOSQUITTO_LIBRARY NAMES mosquitto libmosquitto)
    if(MOSQUITTO_INCLUDE_DIR AND MOSQUITTO_LIBRARY)
        add_library(mosquitto_imported UNKNOWN IMPORTED)
        set_target_properties(mosquitto_imported PROPERTIES
            IMPORTED_LOCATION "${MOSQUITTO_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${MOSQUITTO_INCLUDE_DIR}")
        set(MOSQUITTO_TARGET mosquitto_imported)
        message(STATUS "找到 libmosquitto（经由 find_library）"
                       "头文件=${MOSQUITTO_INCLUDE_DIR} 库=${MOSQUITTO_LIBRARY}")
    endif()
endif()

if(NOT MOSQUITTO_TARGET)
    message(FATAL_ERROR
        "找不到 libmosquitto。\n"
        "  如果你用 MSYS2 UCRT64，请执行：\n"
        "    pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-mosquitto\n"
        "  然后用【MSYS2 UCRT64】终端重新运行 cmake（注意不是 MINGW64 终端）。")
endif()

# ---------------------------------------------------------------------------
# 核心静态库 iotd_core
#   为什么先编成"库"而不是直接编 exe？
#   因为库可以被多个程序复用：device_sim 用它、iot-debug 用它、tests 也用它。
#   这正是 SDK 的本质——把可复用的能力打包，让别人去调用。
# ---------------------------------------------------------------------------
# 示例数据文件复制到构建目录，方便命令行直接引用
#   （这条必须放在 project() 之后）
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/samples DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

add_library(iotd_core STATIC
    src/json.cpp
    src/logger.cpp
    src/mock_source.cpp
    src/serial_port_source.cpp
    src/mqtt_source.cpp
    src/device_client.cpp
)

# PUBLIC 表示：谁链接 iotd_core，谁就自动获得这个 include 目录和 mosquitto。
# 为什么 mosquitto 也是 PUBLIC？因为 mqtt_source.h 里 #include <mosquitto.h>，
# 使用者包含这个头文件时也要能找到它 —— 它属于"接口的一部分"。
target_include_directories(iotd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(iotd_core PUBLIC ${MOSQUITTO_TARGET})

# ---------------------------------------------------------------------------
# 可执行程序
# ---------------------------------------------------------------------------
add_executable(device_sim examples/device_sim.cpp)
target_link_libraries(device_sim PRIVATE iotd_core)

add_executable(mqtt_demo examples/mqtt_demo.cpp)
target_link_libraries(mqtt_demo PRIVATE iotd_core)

add_executable(iot-debug tools/iot_debug_main.cpp)
target_link_libraries(iot-debug PRIVATE iotd_core)

# ---------------------------------------------------------------------------
# 测试（用 CTest 驱动；现在只有一个最简测试，后面会扩充）
# ---------------------------------------------------------------------------
enable_testing()
add_executable(iotd_tests tests/test_json.cpp)
target_link_libraries(iotd_tests PRIVATE iotd_core)
add_test(NAME json_tests COMMAND iotd_tests)

# ---------------------------------------------------------------------------
# 构建后把 libmosquitto.dll 复制到产物目录
#
# 【这一次我们踩了坑，所以有了这段代码】
# 现象：程序一启动就退出，没有任何输出，退出码 -1073741511。
# 排查步骤（这套方法以后遇到任何"缺 DLL"都能用）：
#   1. -1073741511 转十六进制 = 0xC0000139 = STATUS_ENTRYPOINT_NOT_FOUND
#      → 典型含义就是"某个依赖的 DLL 找不到"
#   2. 用 objdump 看真实依赖（依赖是传递的，不能只看直接依赖）：
#        objdump -p libmosquitto.dll | findstr "DLL Name"
#      结果：它还依赖 libcrypto-3-x64.dll、libssl-3-x64.dll、
#            libwinpthread-1.dll —— 最初只复制 libmosquitto.dll 一个，当然起不来。
#
# 【教训】A 依赖 B，B 又依赖 C 和 D。交付时要一次把所有相关 DLL 都复制过去。
# ---------------------------------------------------------------------------
if(WIN32)
    # 所有需要随程序一起分发的 DLL（含传递依赖）
    set(_dll_names
        libmosquitto.dll          # MQTT 库本身
        libcrypto-3-x64.dll       # OpenSSL 加密库（mosquitto 依赖）
        libssl-3-x64.dll          # OpenSSL TLS 库（mosquitto 依赖）
        libwinpthread-1.dll       # MinGW 线程库
        libstdc++-6.dll           # GCC 的 C++ 标准库
        libgcc_s_seh-1.dll        # GCC 底层运行库
    )

    set(_found_dlls "")
    foreach(_name ${_dll_names})
        find_file(_dll_path_${_name} NAMES ${_name}
                  PATHS "C:/msys64/ucrt64/bin" "C:/msys64/mingw64/bin"
                        "${MOSQUITTO_PREFIX}/bin"
                  NO_DEFAULT_PATH)
        if(_dll_path_${_name})
            list(APPEND _found_dlls "${_dll_path_${_name}}")
        else()
            message(STATUS "未找到 DLL（可能不需要）：${_name}")
        endif()
    endforeach()

    if(_found_dlls)
        add_custom_command(TARGET mqtt_demo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_found_dlls}
                    "$<TARGET_FILE_DIR:mqtt_demo>"
            COMMENT "复制依赖 DLL 到产物目录")
        list(LENGTH _found_dlls _dll_count)
        message(STATUS "将复制 ${_dll_count} 个依赖 DLL 到产物目录")
    else()
        message(WARNING
            "没找到依赖 DLL，双击 exe 会报缺 DLL。\n"
            "  手动把 C:/msys64/ucrt64/bin/ 下的 libmosquitto/libcrypto/libssl/"
            "libwinpthread/libstdc++/libgcc 复制到 build 目录即可。")
    endif()
endif()

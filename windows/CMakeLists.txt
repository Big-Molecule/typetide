cmake_minimum_required(VERSION 3.20)
project(TypeTide VERSION 0.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(TypeTide WIN32
    src/main.cpp
    src/App.cpp
    src/Log.cpp
    src/LocalDiagnostics.cpp
    src/Json.cpp
    src/Language.cpp
    src/Settings.cpp
    src/Credentials.cpp
    src/Clipboard.cpp
    src/Keyboard.cpp
    src/Http.cpp
    src/Translator.cpp
    src/CrashDump.cpp
    src/SelectionCapture.cpp
    src/TextReplacer.cpp
    src/Hooks.cpp
    src/PopupWindow.cpp
    src/SelectionIcon.cpp
    src/SelectionMonitor.cpp
    src/TrayIcon.cpp
    src/SettingsWindow.cpp
    src/ModernFormControls.cpp
    src/LaunchAtLogin.cpp
    src/UpdateChecker.cpp
    src/SelfTest.cpp
    src/TypeTide.rc
    src/TypeTide.manifest
)

# 更新检查指向的 GitHub 仓库（owner/repo）。改自己的 fork 时在这里改，或在
# 配置阶段用 -DTYPETIDE_UPDATE_REPO=owner/repo 覆盖；版本号与之无关。
set(TYPETIDE_UPDATE_REPO "Big-Molecule/typetide" CACHE STRING "owner/repo used by the update checker")

target_compile_definitions(TypeTide PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    TYPETIDE_VERSION_STRING="${PROJECT_VERSION}"
    TYPETIDE_UPDATE_REPO="${TYPETIDE_UPDATE_REPO}"
    TYPETIDE_CONTRACTS_PATH="${CMAKE_SOURCE_DIR}/../contracts/translation-contracts.json"
)

if(MSVC)
    target_compile_options(TypeTide PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(TypeTide PRIVATE
    user32 gdi32 shell32 ole32 oleaut32 uuid comctl32
    winhttp uiautomationcore dwmapi advapi32 shlwapi uxtheme dbghelp
    comdlg32 bcrypt ws2_32
)

include(CTest)
if(BUILD_TESTING)
    add_executable(SettingsLayoutTest tests/settings-layout.cpp)
    target_include_directories(SettingsLayoutTest PRIVATE src)
    target_compile_definitions(SettingsLayoutTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(SettingsLayoutTest PRIVATE user32)
    add_test(NAME settings-layout COMMAND SettingsLayoutTest)
    add_executable(ModernFormControlsTest tests/modern-form-controls.cpp src/ModernFormControls.cpp)
    target_include_directories(ModernFormControlsTest PRIVATE src)
    target_compile_definitions(ModernFormControlsTest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(ModernFormControlsTest PRIVATE /W4 /utf-8 /EHsc)
    endif()
    target_link_libraries(ModernFormControlsTest PRIVATE user32 gdi32 comctl32 dwmapi uxtheme)
    add_test(NAME modern-form-controls COMMAND ModernFormControlsTest)
endif()

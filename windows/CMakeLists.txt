cmake_minimum_required(VERSION 3.20)
project(TypeTide VERSION 0.1.21 LANGUAGES CXX)

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
    src/BuiltInModel.cpp
    src/OllamaModels.cpp
    src/CrashDump.cpp
    src/SelectionCapture.cpp
    src/TextReplacer.cpp
    src/Hooks.cpp
    src/PopupWindow.cpp
    src/SelectionIcon.cpp
    src/SelectionMonitor.cpp
    src/TrayIcon.cpp
    src/SettingsWindow.cpp
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

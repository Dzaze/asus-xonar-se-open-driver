# Tests des contrats v1 (E0-02).
#
# Utilisable de deux façons :
#  - autonome : cmake -S tests/contracts -B build/contracts -G Ninja
#               cmake --build build/contracts && ctest --test-dir build/contracts
#  - intégré  : add_subdirectory(contracts) dans tests/CMakeLists.txt (E0-05).
#
# Aucun test n'installe quoi que ce soit ni ne touche au système (règle S0).
cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(XonarContractsTests LANGUAGES CXX)
    enable_testing()
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

get_filename_component(XONAR_CONTRACTS_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# --- Python : schémas, fixtures, cas négatifs, migration, reproductibilité ---
find_package(Python3 3.12 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME contracts.python
        COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/run_contract_tests.py")
    add_test(NAME contracts.generated_up_to_date
        COMMAND "${Python3_EXECUTABLE}" -I "${XONAR_CONTRACTS_REPO}/contracts/generate.py" --check)
    set_tests_properties(contracts.python PROPERTIES TIMEOUT 300)
else()
    message(STATUS "Python 3.12 absent : tests Python des contrats désactivés")
endif()

# --- C++ : en-tête généré aligné sur dsp-core (en-têtes seulement) ---
add_executable(xonar_contracts_conformance cpp/conformance_main.cpp)
target_include_directories(xonar_contracts_conformance PRIVATE
    "${XONAR_CONTRACTS_REPO}/contracts/generated/cpp/include"
    "${XONAR_CONTRACTS_REPO}/src/dsp-core/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/cpp")
target_compile_features(xonar_contracts_conformance PRIVATE cxx_std_20)
if(COMMAND xonar_apply_warnings)
    xonar_apply_warnings(xonar_contracts_conformance)
elseif(MSVC)
    target_compile_options(xonar_contracts_conformance PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
else()
    target_compile_options(xonar_contracts_conformance PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME contracts.cpp_conformance COMMAND xonar_contracts_conformance)

# --- C# : DTO générés compilés par le csc du .NET Framework (C# 5) ---
# Facultatif : aucun SDK .NET requis ni installé. Absent = test non déclaré.
if(WIN32)
    find_program(XONAR_LEGACY_CSC csc
        PATHS "$ENV{WINDIR}/Microsoft.NET/Framework64/v4.0.30319" NO_DEFAULT_PATH)
    if(XONAR_LEGACY_CSC)
        set(_cs_gen "${XONAR_CONTRACTS_REPO}/contracts/generated/csharp/Xonar.Contracts.V1.g.cs")
        set(_cs_test "${CMAKE_CURRENT_SOURCE_DIR}/csharp/ContractsSmoke.cs")
        set(_cs_exe "${CMAKE_CURRENT_BINARY_DIR}/xonar_contracts_csharp_smoke.exe")
        file(TO_NATIVE_PATH "${_cs_gen}" _cs_gen_n)
        file(TO_NATIVE_PATH "${_cs_test}" _cs_test_n)
        file(TO_NATIVE_PATH "${_cs_exe}" _cs_exe_n)
        add_custom_command(OUTPUT "${_cs_exe}"
            COMMAND "${XONAR_LEGACY_CSC}" /nologo /codepage:65001 /warnaserror+ /target:exe
                    "/out:${_cs_exe_n}" /r:System.Web.Extensions.dll "${_cs_gen_n}" "${_cs_test_n}"
            DEPENDS "${_cs_gen}" "${_cs_test}"
            COMMENT "Compilation C# 5 des DTO générés"
            VERBATIM)
        add_custom_target(xonar_contracts_csharp_smoke ALL DEPENDS "${_cs_exe}")
        add_test(NAME contracts.csharp_smoke
            COMMAND "${_cs_exe}" "${XONAR_CONTRACTS_REPO}/configs/examples/v1/normalized/exemple-section-1.4.json")
    else()
        message(STATUS "csc du .NET Framework introuvable : test C# des contrats non déclaré")
    endif()
endif()

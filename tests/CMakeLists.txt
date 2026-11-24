# Testy jednostkowe i integracyjne (doctest). Kazdy plik *_test.cpp w tym katalogu
# (bez podkatalogow) trafia do vgmplay_tests automatycznie — nie trzeba edytowac tej listy.
# Po dodaniu nowego pliku uruchom ponownie konfiguracje CMake (CONFIGURE_DEPENDS robi to sam).
find_package(doctest CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

file(GLOB VGMPLAY_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
add_executable(vgmplay_tests ${VGMPLAY_TEST_SOURCES})
target_link_libraries(vgmplay_tests PRIVATE vgmplay::options doctest::doctest nlohmann_json::nlohmann_json)
foreach(_m IN LISTS VGMPLAY_MODULES)
    target_link_libraries(vgmplay_tests PRIVATE vgmplay::${_m})
endforeach()
# Dane testowe (np. pliki z generatora) — sciezka dostepna w kodzie testow.
target_compile_definitions(vgmplay_tests PRIVATE VGMPLAY_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

add_test(NAME vgmplay_tests COMMAND vgmplay_tests)

# Testy jednostkowe i integracyjne (doctest). Nowe pliki *_test.cpp dopisuj do listy ponizej.
find_package(doctest CONFIG REQUIRED)

add_executable(vgmplay_tests
    smoke_test.cpp
    contracts_test.cpp
)
target_link_libraries(vgmplay_tests PRIVATE vgmplay::options doctest::doctest)
foreach(_m IN LISTS VGMPLAY_MODULES)
    target_link_libraries(vgmplay_tests PRIVATE vgmplay::${_m})
endforeach()

add_test(NAME vgmplay_tests COMMAND vgmplay_tests)

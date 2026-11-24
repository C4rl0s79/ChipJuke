# Modul: library (ScanJob - skan katalogow we wlasnym watku; metadane + cache - tylko z workera).
# fs_util.hpp to wewnetrzny naglowek modulu settings (zapis atomowy, UTF-8).
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_library STATIC
    scan_job.cpp
    meta.cpp
)
add_library(vgmplay::library ALIAS vgmplay_library)
target_include_directories(vgmplay_library PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(vgmplay_library PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../settings)
target_link_libraries(vgmplay_library
    PUBLIC vgmplay::options vgmplay::format
    PRIVATE vgmplay::loader nlohmann_json::nlohmann_json)

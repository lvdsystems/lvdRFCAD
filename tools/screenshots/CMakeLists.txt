# Generates the screenshots of the user manual (docs/manual/images).
# Usage: manual_screenshots <examples folder> <output folder>
add_executable(manual_screenshots main.cpp)
target_link_libraries(manual_screenshots PRIVATE lvdgui)

# Drawing code shared by the schematic editor, the library browser and the
# symbol editor.
add_library(lvdwidgets STATIC
    SchematicStyle.cpp
    SchematicStyle.h
    SymbolPainter.cpp
    SymbolPainter.h
)
target_link_libraries(lvdwidgets PUBLIC lvdcore Qt6::Widgets)

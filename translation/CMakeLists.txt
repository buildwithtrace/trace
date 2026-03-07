if( UNIX AND KICAD_I18N_UNIX_STRICT_PATH )
    set( KICAD_I18N_PATH ${CMAKE_INSTALL_PREFIX}/share/locale
        CACHE PATH "Location of KiCad i18n files." )
elseif( APPLE )
    set( KICAD_I18N_PATH ${KICAD_DATA}/internat
        CACHE PATH "Location of KiCad i18n files." )
else()
    # Default locale lookup path for Trace
    set( KICAD_I18N_PATH share/trace/internat
        CACHE PATH "Location of Trace i18n files." )
endif()

file(STRINGS pofiles/LINGUAS_INSTALL LANG_ARRAY REGEX "^[^#].*")

# Add the commands to translate each language file
foreach( LANG ${LANG_ARRAY} )
    set( OUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/${LANG}/kicad.mo" )

    translate_language( ${LANG} ${OUT_FILE} )

    # Keep a list of the language files that are created to add to the target
    list( APPEND LANG_FILES "${OUT_FILE}" )
endforeach()

# Add the target that executes the translation commands
add_custom_target( i18n ALL
                   DEPENDS ${LANG_FILES} )

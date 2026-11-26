# Optional supporting files must never replace the generated default library.
file(MAKE_DIRECTORY "${DESTINATION}")
if(IS_DIRECTORY "${SOURCE}" AND NOT SOURCE STREQUAL DESTINATION)
    file(COPY "${SOURCE}/" DESTINATION "${DESTINATION}" PATTERN "lincupl.dl" EXCLUDE)
endif()

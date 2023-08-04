# - Find ZMusic
# Find the native ZMusic includes and library
#
#  ZMUSIC_INCLUDE_DIR - where to find zmusic.h
#  ZMUSIC_LIBRARY     - Library when using ZMusic.
#  ZMUSIC_FOUND       - True if ZMusic found.

IF(ZMUSIC_INCLUDE_DIR AND ZMUSIC_LIBRARY)
  # Already in cache, be silent
  SET(ZMUSIC_FIND_QUIETLY TRUE)
ENDIF(ZMUSIC_INCLUDE_DIR AND ZMUSIC_LIBRARY)

FIND_PATH(ZMUSIC_INCLUDE_DIR zmusic.h
          PATHS "${ZMUSIC_DIR}" ENV ZMUSIC_DIR
          )

FIND_LIBRARY(ZMUSIC_LIBRARY NAMES ZMusic zmusic zmusic.lib
             PATHS "${ZMUSIC_DIR}" ENV ZMUSIC_DIR
             )

# MARK_AS_ADVANCED(ZMUSIC_LIBRARY ZMUSIC_INCLUDE_DIR)

# handle the QUIETLY and REQUIRED arguments and set ZMUSIC_FOUND to TRUE if
# all listed variables are TRUE
INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(ZMUSIC DEFAULT_MSG ZMUSIC_LIBRARY ZMUSIC_INCLUDE_DIR)

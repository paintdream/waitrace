CMAKE_MINIMUM_REQUIRED (VERSION 2.8)
PROJECT (waitrace)

OPTION (STATIC_VC_RUNTIME "Use static VC runtime" ON)

IF (MSVC)
	# Default to statically-linked runtime.
	IF (STATIC_VC_RUNTIME)
		SET (MSVC_RUNTIME "static")
	ELSE()
		SET (MSVC_RUNTIME "dynamic")
	ENDIF ()
	# SET compiler options.
	SET (variables
		CMAKE_C_FLAGS_DEBUG
		CMAKE_C_FLAGS_MINSIZEREL
		CMAKE_C_FLAGS_RELEASE
		CMAKE_C_FLAGS_RELWITHDEBINFO
		CMAKE_CXX_FLAGS_DEBUG
		CMAKE_CXX_FLAGS_MINSIZEREL
		CMAKE_CXX_FLAGS_RELEASE
		CMAKE_CXX_FLAGS_RELWITHDEBINFO
	)

	ADD_DEFINITIONS (-D_HAS_EXCEPTIONS=0)
	IF (${MSVC_RUNTIME} STREQUAL "static")
		# disable RTTI & Exceptions
		SET (CMAKE_CXX_FLAGS, "/DWIN32 /D_WINDOWS /W3")
		MESSAGE (STATUS
			"MSVC -> forcing use of statically-linked runtime."
		)
		FOREACH (variable ${variables})
			IF (${variable} MATCHES "/MD")
				string(REGEX REPLACE "/MD" "/MT" ${variable} "${${variable}}")
			ENDIF ()
		ENDFOREACH ()
	ELSE()
		MESSAGE(STATUS
			"MSVC -> forcing use of dynamically-linked runtime."
		)
		FOREACH (variable ${variables})
			IF (${variable} MATCHES "/MT")
				string(REGEX REPLACE "/MT" "/MD" ${variable} "${${variable}}")
			ENDIF ()
		ENDFOREACH ()
	ENDIF ()
ENDIF ()


SET (STDLIB stdc++)

SET (CMAKE_CXX_STANDARD 14)
# SET (CMAKE_CXX_STANDARD 20)
# IF (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
# 	ADD_DEFINITIONS(-fcoroutines-ts -stdlib=libc++)
# 	SET (STDLIB c++)
# ELSEIF (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
# 	# ADD_DEFINITIONS(-fcoroutines)
# ENDIF (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")

INCLUDE_DIRECTORIES ("${PROJECT_BINARY_DIR}")
STRING (REPLACE "/" "\\" LOCAL_SOURCE_DIR "${PROJECT_SOURCE_DIR}")

FILE (GLOB_RECURSE WAITRACE_SRC
	"${PROJECT_SOURCE_DIR}/ref/grid_dispatcher/*.h"
	"${PROJECT_SOURCE_DIR}/src/*.cpp"
	"${PROJECT_SOURCE_DIR}/src/*.h"
)

FOREACH (FILE ${WAITRACE_SRC}) 
	GET_FILENAME_COMPONENT(PARENT_DIR "${FILE}" PATH)

	# skip src or include and changes /'s to \\'s
	STRING(REGEX REPLACE "(\\./)?(src|include)/?" "" GROUP "${PARENT_DIR}")
	STRING(REPLACE "/" "\\" GROUP "${GROUP}")
	STRING(REPLACE "${LOCAL_SOURCE_DIR}" "" GROUP "${GROUP}")
	SOURCE_GROUP("${GROUP}" FILES "${FILE}")
ENDFOREACH (FILE ${WAITRACE_SRC})

ADD_LIBRARY (waitrace SHARED ${WAITRACE_SRC})

IF (NOT MSVC)
	TARGET_LINK_LIBRARIES(waitrace m ${STDLIB} pthread)
ENDIF (NOT MSVC)

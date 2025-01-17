# These modules can always be built on all platforms.
file(GLOB INSPIRCD_MODULES
	"${INSPIRCD_BASE}/src/coremods/core_*"
	"${INSPIRCD_BASE}/src/modules/m_*")

# These modules have their dependencies provided by Windows.
list(APPEND INSPIRCD_MODULES
	"${INSPIRCD_BASE}/src/modules/extra/m_ldap.cpp")

if(EXISTS "${CMAKE_BINARY_DIR}/conanbuildinfo.cmake")
	include("${CMAKE_BINARY_DIR}/conanbuildinfo.cmake")

	function(enable_extra NAME PACKAGE)
		if(DEFINED "CONAN_${PACKAGE}_ROOT")
			message("Enabling the ${NAME} module")
			list(APPEND INSPIRCD_MODULES "${INSPIRCD_BASE}/src/modules/extra/m_${NAME}.cpp")
			set(INSPIRCD_MODULES ${INSPIRCD_MODULES} PARENT_SCOPE)
		else()
			message("Unable to enable the ${NAME} module (missing library)")
		endif()
	endfunction()

	enable_extra("argon2" "ARGON2")
	enable_extra("geo_maxmind" "LIBMAXMINDDB")
	enable_extra("log_json" "RAPIDJSON")
	enable_extra("mysql" "LIBMYSQLCLIENT")
	enable_extra("pgsql" "LIBPQ")
	enable_extra("regex_pcre" "PCRE2")
	enable_extra("regex_posix" "PCRE2")
	enable_extra("regex_re2" "RE2")
	enable_extra("ssl_mbedtls" "MBEDTLS")
	enable_extra("ssl_openssl" "OPENSSL")
	enable_extra("sqlite3" "SQLITE3")

	link_directories("${CMAKE_BINARY_DIR}/extradll" "${CMAKE_BINARY_DIR}/extralib")
	file(GLOB EXTRA_DLLS "${CMAKE_BINARY_DIR}/extradll/*.dll")
	install(FILES ${EXTRA_DLLS} DESTINATION .)

	conan_basic_setup(TARGETS)
else()
	message("Unable to build extras: conanbuildinfo.cmake does not exist in the build directory!")
endif()

list(SORT INSPIRCD_MODULES)

add_definitions("-DDLL_BUILD")

foreach(MODULE_NAME ${INSPIRCD_MODULES})
	if(IS_DIRECTORY "${MODULE_NAME}")
		string(REGEX REPLACE "^.*[/\\](.*)$" "\\1" BASE_NAME ${MODULE_NAME})
	else()
		string(REGEX REPLACE "^.*[/\\](.*).cpp$" "\\1" BASE_NAME ${MODULE_NAME})
	endif()

	if(IS_DIRECTORY "${MODULE_NAME}")
		file(GLOB MODULES_SUBDIR_SRCS "${MODULE_NAME}/*.cpp")
		list(SORT MODULES_SUBDIR_SRCS)
		add_library(${BASE_NAME} MODULE ${MODULES_SUBDIR_SRCS})
	else()
		add_library(${BASE_NAME} MODULE ${MODULE_NAME})
	endif()

	# Generate the module and set its linker flags, also set it to depend on the main executable to be built beforehand
	target_link_libraries(${BASE_NAME} inspircd)
	add_dependencies(${BASE_NAME} inspircd)
	if(MSVC)
		target_link_libraries(${BASE_NAME} win32_memory)
		add_dependencies(${BASE_NAME} win32_memory)
	endif()

	if(CONAN_CXX_FLAGS)
		conan_target_link_libraries(${BASE_NAME})
	endif()

	set_target_properties(${BASE_NAME} PROPERTIES
		PREFIX ""
		FOLDER "Modules"
		COMPILE_DEFINITIONS "FMT_SHARED;MODNAME=\"${BASE_NAME}\""
	)

	# Set the module to be installed to the module directory
	install(TARGETS ${BASE_NAME} LIBRARY DESTINATION ${MODULE_DIR})
endforeach()

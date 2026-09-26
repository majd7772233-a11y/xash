cmake_minimum_required(VERSION 3.5)

project(MAINUI)

set(CMAKE_CXX_STANDARD 11)

option(MAINUI_USE_CUSTOM_FONT_RENDER
	"Use custom font rendering"
	ON
)

option(MAINUI_USE_STB
	"Use stb_truetype.h for rendering(*nix-only)"
	OFF
)

option(MAINUI_FONT_SCALE
	"Scale fonts by height"
	OFF
)

if(NOT XASH_SDK)
	set(XASH_SDK "sdk_includes/")
endif()

file(GLOB MAINUI_CONTROLS_SOURCES
	"controls/*.cpp"
)

file(GLOB MAINUI_MENUS_SOURCES
	"menus/*.cpp"
	"menus/dynamic/*.cpp"
)

file(GLOB MAINUI_FONT_RENDER_SOURCES
	"font/*.cpp"
)

file(GLOB MAINUI_SOURCES
	"miniutl/*.cpp"
	"*.cpp"
)

add_library(
	menu
	SHARED
	${MAINUI_CONTROLS_SOURCES}
	${MAINUI_MENUS_SOURCES}
	${MAINUI_FONT_RENDER_SOURCES}
	${MAINUI_SOURCES}
)

if(NOT WIN32 AND NOT MINGW)
	add_compile_options(
		-Wall
		-Wextra
		-Wno-unused-parameter
		-Wno-unused-variable
	)
endif()

if(MAINUI_FONT_SCALE)
	add_definitions(
		-DMAINUI_FONT_SCALE
	)
endif()

add_definitions(
	-DSTDINT_H=<cstdint>
)

if(ANDROID)
	set(MAINUI_USE_STB TRUE)
endif()

include_directories(
	${XASH_SDK}/common
	${XASH_SDK}/engine
	${XASH_SDK}/pm_shared
	${XASH_SDK}/public
	.
	controls/
	menus/
	miniutl/
	font/
	model/
)

if(MSVC)
	add_definitions(
		-D_CRT_SECURE_NO_WARNINGS
	)
endif()

if(MAINUI_USE_CUSTOM_FONT_RENDER)

	if(NOT WIN32)

		if(MAINUI_USE_STB)

			add_definitions(
				-DMAINUI_USE_STB
			)

		else()

			find_package(PkgConfig)

			pkg_check_modules(
				FT2
				REQUIRED
				freetype2
			)

			include_directories(
				${FT2_INCLUDE_DIRS}
			)

			target_link_libraries(
				menu
				${FT2_LIBRARIES}
			)

			add_definitions(
				-DMAINUI_USE_FREETYPE
			)

		endif()

	endif()

	add_definitions(
		-DMAINUI_USE_CUSTOM_FONT_RENDER
	)

endif()

if(NOT BUILD_AS_PART_OF_ENGINE)

	set_target_postfix(menu)

else()

	set(GAMEDIR ".")
	set(CLIENT_INSTALL_DIR ".")

endif()

install(
	TARGETS menu
	DESTINATION
	"${GAMEDIR}/${CLIENT_INSTALL_DIR}"
)

if(MSVC)

	install(
		FILES
		$<TARGET_PDB_FILE:menu>
		DESTINATION
		"${GAMEDIR}/${CLIENT_INSTALL_DIR}"
		OPTIONAL
	)

endif()
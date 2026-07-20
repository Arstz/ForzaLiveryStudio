cmake_minimum_required(VERSION 3.24)

project(ForzaLiveryStudio LANGUAGES CXX C)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)
find_package(ZLIB REQUIRED)
qt_standard_project_setup()

# Vendored potrace bitmap tracer (potracelib core only), GPLv2-or-later, which
# is compatible with this project's AGPLv3. See third_party/potrace/COPYING.
add_library(potrace STATIC
    third_party/potrace/potracelib.c
    third_party/potrace/curve.c
    third_party/potrace/trace.c
    third_party/potrace/decompose.c
)
target_include_directories(potrace PUBLIC third_party/potrace)
target_compile_definitions(potrace PRIVATE VERSION="1.16" HAVE_INTTYPES_H=1 HAVE_STDINT_H=1)
set_target_properties(potrace PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

add_library(fls_core STATIC
    src/core/binary_io.cpp
    src/core/binary_io.h
    src/core/cgroup_codec.cpp
    src/core/cgroup_codec.h
    src/core/core_types.cpp
    src/core/core_types.h
    src/core/fh6_core.cpp
    src/core/fh6_core.h
    src/core/fm_codec.cpp
    src/core/fm_codec.h
    src/core/header_codec.cpp
    src/core/header_codec.h
    src/core/layer.cpp
    src/core/layer.h
    src/core/visual_container.h
    src/core/flat_payload.cpp
    src/core/flat_payload.h
    src/core/nested_payload.cpp
    src/core/nested_payload.h
    src/core/livery_codec.cpp
    src/core/livery_codec.h
    src/core/livery_masks.cpp
    src/core/livery_masks.h
    src/core/car_scene.cpp
    src/core/car_scene.h
    src/core/matrix_math.cpp
    src/core/matrix_math.h
    src/core/model_bundle.cpp
    src/core/model_bundle.h
    src/core/model_geometry.cpp
    src/core/model_geometry.h
    src/core/model_material.cpp
    src/core/model_material.h
    src/core/project_codec.cpp
    src/core/project_codec.h
    src/core/raster_decals.cpp
    src/core/raster_decals.h
    src/core/scene_codec.cpp
    src/core/scene_codec.h
    src/core/shape_registry.cpp
    src/core/shape_registry.h
    src/core/swatchbin.cpp
    src/core/swatchbin.h
    src/core/vinyl_decoder.cpp
    src/core/vinyl_decoder.h
    src/core/vinyl_flattener.cpp
    src/core/vinyl_flattener.h
    src/core/zip_extract.cpp
    src/core/zip_extract.h
)

target_link_libraries(fls_core
    PUBLIC
        Qt6::Core
        ZLIB::ZLIB
)

if(WIN32)
    target_link_libraries(fls_core PUBLIC d3d11 d3dcompiler)
endif()

target_include_directories(fls_core PUBLIC src/core)

option(FH6_PRIVACY_POLICY "Deny importing locked game content in public builds" ON)
option(FH6_BUILD_HELPER_TOOLS "Build console helper tools" OFF)
option(FH6_BUILD_LIVERY_COMPARE "Build livery compare helper when helper tools are enabled" OFF)
option(ENFORCE_SHAPE_LIMITS "Enforce per-section livery shape limits during export" ON)

target_compile_definitions(fls_core
    PUBLIC
        FH6_PRIVACY_POLICY=$<BOOL:${FH6_PRIVACY_POLICY}>
        ENFORCE_SHAPE_LIMITS=$<BOOL:${ENFORCE_SHAPE_LIMITS}>
)

if(FH6_BUILD_HELPER_TOOLS AND FH6_BUILD_LIVERY_COMPARE)
    # Console harness: diff a decoded C_livery against the in-game per-section
    # C_group exports (the ground truth for group decoding). Core-only, no GUI deps.
    add_executable(fh6_livery_compare
        tools/livery_compare/main.cpp
    )
    target_link_libraries(fh6_livery_compare PRIVATE fls_core Qt6::Core)
endif()

if(FH6_BUILD_HELPER_TOOLS)
    # Console harness: decode a .modelbin and print mesh/vertex/index counts and bounds
    # to sanity-check the geometry decoder against ForzaTechStudio. Core-only, no GUI.
    add_executable(fh6_model_dump
        tools/model_dump/main.cpp
    )
    target_link_libraries(fh6_model_dump PRIVATE fls_core Qt6::Core)

    # Console harness: decode built-in decal swatchbins into one indexed RGBA pack.
    add_executable(fh6_pack_decals
        tools/pack_decals/main.cpp
    )
    target_link_libraries(fh6_pack_decals PRIVATE fls_core Qt6::Core)

endif()

set(FORZA_LIVERY_STUDIO_TARGET ForzaLiveryStudio)

qt_add_executable(${FORZA_LIVERY_STUDIO_TARGET} WIN32
    src/main.cpp
    src/gui/app/car_registry.cpp
    src/gui/app/car_registry.h
    src/gui/app/dock_chrome.cpp
    src/gui/app/dock_chrome.h
    src/gui/app/gui_assets.cpp
    src/gui/app/gui_assets.h
    src/gui/app/gui_constants.h
    src/gui/app/main_window.cpp
    src/gui/app/main_window.h
    src/gui/app/main_window_internal.h
    src/gui/app/main_window_edit.cpp
    src/gui/app/main_window_io.cpp
    src/gui/app/main_window_project.cpp
    src/gui/app/main_window_ui.cpp
    src/gui/app/perf_utils.h
    src/gui/app/settings_dialog.cpp
    src/gui/app/settings_dialog.h
    src/gui/app/theme_manager.cpp
    src/gui/app/theme_manager.h
    src/gui/state/editor_state.cpp
    src/gui/state/editor_state.h
    src/gui/state/editor_state_clipboard.cpp
    src/gui/state/editor_state_grouping.cpp
    src/gui/state/editor_state_selection.cpp
    src/gui/state/editor_state_snapshot.cpp
    src/gui/state/layer_tree_model.cpp
    src/gui/state/layer_tree_model.h
    src/gui/state/scene_view.h
    src/gui/canvas/canvas_tools.cpp
    src/gui/canvas/canvas_tools.h
    src/gui/canvas/car_model_renderer.cpp
    src/gui/canvas/car_model_renderer.h
    src/gui/canvas/bucket_fill.cpp
    src/gui/canvas/bucket_fill.h
    src/gui/canvas/drag_cursors.cpp
    src/gui/canvas/drag_cursors.h
    src/gui/canvas/native_shape_renderer.cpp
    src/gui/canvas/native_shape_renderer.h
    src/gui/canvas/lining_fill.cpp
    src/gui/canvas/lining_fill.h
    src/gui/canvas/pen_fill.cpp
    src/gui/canvas/pen_fill.h
    src/gui/canvas/polygon_mesh.cpp
    src/gui/canvas/polygon_mesh.h
    src/gui/canvas/region_extract.cpp
    src/gui/canvas/region_extract.h
    src/gui/canvas/region_fill.cpp
    src/gui/canvas/region_fill.h
    src/gui/canvas/project_canvas.cpp
    src/gui/canvas/project_canvas_bucket.cpp
    src/gui/canvas/project_canvas_pen.cpp
    src/gui/canvas/project_canvas.h
    src/gui/canvas/project_canvas_internal.h
    src/gui/canvas/project_canvas_hit.cpp
    src/gui/canvas/project_canvas_drag.cpp
    src/gui/canvas/project_canvas_cursor.cpp
    src/gui/canvas/project_canvas_paint.cpp
    src/gui/canvas/project_canvas_events.cpp
    src/gui/widgets/car_preview_widget.cpp
    src/gui/widgets/car_preview_widget.h
    src/gui/widgets/clipboard_buffer_widget.cpp
    src/gui/widgets/clipboard_buffer_widget.h
    src/gui/widgets/color_palette_widget.cpp
    src/gui/widgets/color_palette_widget.h
    src/gui/widgets/font_glyphs.cpp
    src/gui/widgets/font_glyphs.h
    src/gui/widgets/header_metadata_widget.cpp
    src/gui/widgets/header_metadata_widget.h
    src/gui/widgets/image_io.cpp
    src/gui/widgets/image_io.h
    src/gui/widgets/image_preprocess_dialog.cpp
    src/gui/widgets/image_preprocess_dialog.h
    src/gui/widgets/image_preprocessor.cpp
    src/gui/widgets/image_preprocessor.h
    src/gui/widgets/import_asset_dialog.cpp
    src/gui/widgets/import_asset_dialog.h
    src/gui/widgets/import_locations.cpp
    src/gui/widgets/import_locations.h
    src/gui/widgets/layer_state_delegate.cpp
    src/gui/widgets/layer_state_delegate.h
    src/gui/widgets/layer_tree_view.cpp
    src/gui/widgets/layer_tree_view.h
    src/gui/widgets/livery_section_bar.cpp
    src/gui/widgets/livery_section_bar.h
    src/gui/widgets/property_panel.cpp
    src/gui/widgets/property_panel.h
    src/gui/widgets/shape_geometry_store.cpp
    src/gui/widgets/shape_geometry_store.h
    src/gui/widgets/shape_name_store.cpp
    src/gui/widgets/shape_name_store.h
    src/gui/widgets/shapes_browser_widget.cpp
    src/gui/widgets/shapes_browser_widget.h
)

target_include_directories(${FORZA_LIVERY_STUDIO_TARGET} PRIVATE
    src
    src/gui/app
    src/gui/state
    src/gui/canvas
    src/gui/widgets
)

target_link_libraries(${FORZA_LIVERY_STUDIO_TARGET}
    PRIVATE
        fls_core
        potrace
        Qt6::Gui
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
)

if(BUILD_TESTING)
    add_executable(fls_guide_preprocess_metadata_tests
        tools/guide_preprocess_metadata_test.cpp
    )
    target_link_libraries(fls_guide_preprocess_metadata_tests PRIVATE
        fls_core
        Qt6::Core
    )
    add_test(NAME guide_preprocess_metadata COMMAND fls_guide_preprocess_metadata_tests)

    add_executable(fls_image_preprocessor_tests
        tools/image_preprocessor_test.cpp
        src/gui/widgets/image_preprocessor.cpp
    )
    target_include_directories(fls_image_preprocessor_tests PRIVATE
        src/gui/widgets
    )
    target_link_libraries(fls_image_preprocessor_tests PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    add_test(NAME image_preprocessor COMMAND fls_image_preprocessor_tests)

    add_executable(fls_region_pen_conversion_tests
        tools/region_pen_conversion_test.cpp
        src/gui/canvas/bucket_fill.cpp
        src/gui/canvas/lining_fill.cpp
        src/gui/canvas/pen_fill.cpp
        src/gui/canvas/polygon_mesh.cpp
        src/gui/canvas/region_extract.cpp
        src/gui/canvas/region_fill.cpp
        src/gui/widgets/shape_geometry_store.cpp
    )
    target_include_directories(fls_region_pen_conversion_tests PRIVATE
        src/gui/canvas
        src/gui/widgets
    )
    target_link_libraries(fls_region_pen_conversion_tests PRIVATE
        Qt6::Core
        Qt6::Gui
        ZLIB::ZLIB
        potrace
    )
    add_test(NAME region_pen_conversion COMMAND fls_region_pen_conversion_tests)
endif()

set(FH6_GUI_ASSETS
    Cursor.xpm
    CursorPipette.xpm
    MenuCopy.xpm
    MenuCut.xpm
    MenuDelete.xpm
    MenuExit.xpm
    MenuExportFlat.xpm
    MenuFoldAllGroups.xpm
    MenuGroup.xpm
    ImportCar.xpm
    ImportGuide.xpm
    MenuOpenCGroup.xpm
    MenuOpenFolder.xpm
    MenuPaste.xpm
    MenuRevert.xpm
    MenuUngroupFlat.xpm
    PropertyColor.xpm
    PropertyDebug.xpm
    PropertyInvisible.xpm
    PropertyLocked.xpm
    PropertyMask.xpm
    PropertyName.xpm
    PropertyShapeID.xpm
    PropertyUnlocked.xpm
    PropertyVisible.xpm
    PropertyXY.xpm
    ShapeBrowserFavOff.xpm
    ShapeBrowserFavOn.xpm
    ShapeBrowserSearch.xpm
    ToolbarMarquee.xpm
    ToolbarMove.xpm
    ToolbarPen.xpm
    ToolbarRotate.xpm
    ToolbarScale.xpm
    ToolbarSelect.xpm
    ToolbarSkew.xpm
    ToolPipette.xpm
    ToolBucket.xpm
    ToolRotateE.xpm
    ToolRotateN.xpm
    ToolRotateNE.xpm
    ToolRotateNW.xpm
    ToolRotateS.xpm
    ToolRotateSE.xpm
    ToolRotateSW.xpm
    ToolRotateW.xpm
    ToolScaleNESW.xpm
    ToolScaleNWSE.xpm
    ToolScaleX.xpm
    ToolScaleY.xpm
    ToolTransformSkew.xpm
    WidgetBuffer.xpm
    WidgetLayers.xpm
    WidgetProject.xpm
    WidgetProperties.xpm
    WidgetShapesBrowser.xpm
)

set(FH6_GUI_ASSET_SOURCES)
foreach(asset_name IN LISTS FH6_GUI_ASSETS)
    list(APPEND FH6_GUI_ASSET_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/assets/${asset_name}")
endforeach()

add_custom_command(TARGET ${FORZA_LIVERY_STUDIO_TARGET} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E remove_directory
        "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>/assets"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${FH6_GUI_ASSET_SOURCES}
        "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>/assets"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>/assets/vector"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/vector/shape_geometry.json.gz"
        "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>/assets/vector/shape_geometry.json.gz"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/vector/shape_names.json"
        "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>/assets/vector/shape_names.json"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>/assets/raster"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/raster/decal_textures.bin"
        "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>/assets/raster/decal_textures.bin"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>/assets/cars"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/cars/car_ids.json"
        "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>/assets/cars/car_ids.json"
    COMMENT "Deploying used GUI XPM assets"
)

if(NOT FH6_BUILD_HELPER_TOOLS)
    add_custom_command(TARGET ${FORZA_LIVERY_STUDIO_TARGET} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E rm -f
            "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>/fh6_livery_compare.exe"
            "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>/fh6_model_dump.exe"
            "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>/fh6_pack_decals.exe"
        COMMENT "Removing disabled helper tools from release output"
    )
elseif(NOT FH6_BUILD_LIVERY_COMPARE)
    add_custom_command(TARGET ${FORZA_LIVERY_STUDIO_TARGET} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E rm -f
            "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>/fh6_livery_compare.exe"
        COMMENT "Removing disabled livery compare helper from release output"
    )
endif()

if(WIN32)
    target_link_libraries(${FORZA_LIVERY_STUDIO_TARGET} PRIVATE windowscodecs ole32)
    add_custom_command(TARGET ${FORZA_LIVERY_STUDIO_TARGET} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>/platforms"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${Qt6_DIR}/../../Qt6/plugins/platforms/qwindows.dll"
            "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>/platforms/qwindows.dll"
        COMMENT "Deploying Qt platform plugin for ${FORZA_LIVERY_STUDIO_TARGET}"
    )
    add_custom_command(TARGET ${FORZA_LIVERY_STUDIO_TARGET} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E rm -f
            "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>/imageformats/qico.dll"
        COMMENT "Removing redundant Qt ICO imageformat plugin"
    )
    set(QT_WEBP_PLUGIN "${Qt6_DIR}/../../Qt6/plugins/imageformats/qwebp.dll")
    if(NOT EXISTS "${QT_WEBP_PLUGIN}")
        message(FATAL_ERROR "Qt WebP imageformat plugin was not found. Install qtimageformats with the webp feature, e.g. vcpkg install qtimageformats[webp]:x64-windows")
    endif()
    add_custom_command(TARGET ${FORZA_LIVERY_STUDIO_TARGET} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>/imageformats"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${QT_WEBP_PLUGIN}"
            "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>/imageformats/qwebp.dll"
        COMMENT "Deploying Qt WEBP imageformat plugin"
    )
    set(QT_WEBP_RUNTIME_DLLS)
    foreach(webp_dll IN ITEMS libwebp.dll libwebpdecoder.dll libwebpdemux.dll libwebpmux.dll libsharpyuv.dll)
        if(EXISTS "${Qt6_DIR}/../../bin/${webp_dll}")
            list(APPEND QT_WEBP_RUNTIME_DLLS "${Qt6_DIR}/../../bin/${webp_dll}")
        endif()
    endforeach()
    if(QT_WEBP_RUNTIME_DLLS)
        add_custom_command(TARGET ${FORZA_LIVERY_STUDIO_TARGET} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                ${QT_WEBP_RUNTIME_DLLS}
                "$<TARGET_FILE_DIR:${FORZA_LIVERY_STUDIO_TARGET}>"
            COMMENT "Deploying Qt WEBP runtime dependencies"
        )
    endif()
endif()

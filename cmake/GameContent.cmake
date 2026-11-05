# The files of a game that are still Git LFS pointers, relative to the game. A game with
# any is left out of the build, since AssetCooker fails on pointers. Each pointer becomes
# a configure dependency, so `git lfs pull` replacing it reconfigures on the next build.
function(game_lfs_pointers game_dir out_var)
    file(GLOB_RECURSE files LIST_DIRECTORIES false "${game_dir}/*")
    # TerrainTool writes releases on every build; they are never pointers
    list(FILTER files EXCLUDE REGEX "/terrain/releases/")

    set(pointers "")
    foreach(path IN LISTS files)
        # the LFS spec keeps pointers under 1024 bytes
        file(SIZE "${path}" size)
        if(size GREATER_EQUAL 1024)
            continue()
        endif()
        file(READ "${path}" head LIMIT 42)
        if(head MATCHES "^version https://git-lfs")
            file(RELATIVE_PATH relative "${game_dir}" "${path}")
            list(APPEND pointers "${relative}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
        endif()
    endforeach()
    set(${out_var} "${pointers}" PARENT_SCOPE)
endfunction()

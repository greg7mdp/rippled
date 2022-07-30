#[===================================================================[
   NIH dep: gtl
   this library is header-only, thus is an INTERFACE lib in CMake.
#]===================================================================]

find_package (gtl QUIET)
if (NOT TARGET gtl)
  FetchContent_Declare(
    gtl
    GIT_REPOSITORY https://github.com/greg7mdp/gtl.git
    GIT_TAG        22eba92aa8c82355ddcdf58d9a09c7d994acfd7b
  )
  FetchContent_MakeAvailable(gtl)
endif ()

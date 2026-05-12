cmake_minimum_required(VERSION 3.20)

set(TUR_SRCDIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(TUR_OUTDIR "${CMAKE_CURRENT_BINARY_DIR}")

add_custom_command(
  OUTPUT "${TUR_OUTDIR}/minikanren_gen.c"
  COMMAND /bin/sh -c "$<TARGET_FILE:tur> emit-c \"${TUR_SRCDIR}/main.tur\" > \"${TUR_OUTDIR}/minikanren_gen.c\""
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  DEPENDS tur "${TUR_SRCDIR}/main.tur"
  COMMENT "Compiling minikanren/main.tur -> minikanren_gen.c"
  VERBATIM
)

add_executable(minikanren EXCLUDE_FROM_ALL
  "${TUR_OUTDIR}/minikanren_gen.c"
)

target_compile_options(minikanren PRIVATE -w)

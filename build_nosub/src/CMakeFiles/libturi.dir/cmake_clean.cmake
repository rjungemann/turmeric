file(REMOVE_RECURSE
  "libturi.a"
  "libturi.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang ASM C)
  include(CMakeFiles/libturi.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()

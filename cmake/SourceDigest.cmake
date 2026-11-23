# grparse_source_digest(<out_var> <root>)
#
# The parse identity's build digest (Document.parse.build): SHA-256 over a
# sorted manifest of "<relative path> <SHA-256 of the file>" lines, one per
# file under <root> that can change what a parse produces. A function, not
# inline code, so tests/source_digest_test.cmake can run the same
# computation over two scratch trees and prove that one changed byte moves
# the digest.
function(grparse_source_digest out_var root)
  file(GLOB_RECURSE identity_sources LIST_DIRECTORIES false
    RELATIVE "${root}"
    "${root}/src/*"
    "${root}/include/*"
    "${root}/collectors/*.proto"
    "${root}/backends/*.proto"
    "${root}/patches/*"
    "${root}/cmake/*"
    "${root}/*.proto")
  if(EXISTS "${root}/CMakeLists.txt")
    list(APPEND identity_sources CMakeLists.txt)
  endif()
  list(SORT identity_sources)
  set(manifest "")
  foreach(identity_source IN LISTS identity_sources)
    file(SHA256 "${root}/${identity_source}" identity_digest)
    string(APPEND manifest "${identity_source} ${identity_digest}\n")
  endforeach()
  string(SHA256 digest "${manifest}")
  set(${out_var} "${digest}" PARENT_SCOPE)
endfunction()

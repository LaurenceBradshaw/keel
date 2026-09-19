# keel_stl: the standard library, written in Keel and compiled by keelc.
#
# Empty until M8: it needs modules to be more than one file, and a growable container needs the
# many-item pointer. The directory exists now so that the shape of the repository does not have to
# change when it arrives.
add_subdirectory(src)
add_subdirectory(test)

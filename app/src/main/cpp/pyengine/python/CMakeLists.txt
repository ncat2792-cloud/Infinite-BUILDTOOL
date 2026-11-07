cmake_minimum_required(VERSION 3.22)
project(pyloader_python27_compiler C)

# A private, compiler-only CPython runtime. Nothing from this interpreter is
# passed to the game. Only copied marshal bytes cross that boundary.
set(_python_objects abstract boolobject bufferobject bytearrayobject bytes_methods
    capsule cellobject classobject cobject codeobject complexobject descrobject
    dictobject enumobject exceptions fileobject floatobject frameobject funcobject
    genobject intobject iterobject listobject longobject memoryobject methodobject
    moduleobject object obmalloc rangeobject setobject sliceobject stringobject
    structseq tupleobject typeobject unicodectype unicodeobject weakrefobject)
set(_python_core _warnings asdl ast atof bltinmodule ceval codecs compile dtoa
    errors formatter_string formatter_unicode future getargs getcompiler getcopyright getplatform getversion graminit
    import importdl marshal modsupport mysnprintf mystrtoul pyarena pyctype pyfpe
    pymath pystate pystrcmp pystrtod Python-ast pythonrun random sigcheck
    structmember symtable sysmodule thread traceback peephole)
set(_python_parser acceler bitset grammar grammar1 listnode myreadline node parser
    parsetok tokenizer intrcheck)
set(_python_sources CompilerConfig.c CompilerEntry.c CompilerEncodings.c CompilerGameObjects.c)
foreach(_file IN LISTS _python_objects)
    list(APPEND _python_sources Objects/${_file}.c)
endforeach()
foreach(_file IN LISTS _python_core)
    list(APPEND _python_sources Python/${_file}.c)
endforeach()
foreach(_file IN LISTS _python_parser)
    list(APPEND _python_sources Parser/${_file}.c)
endforeach()
list(APPEND _python_sources Modules/gcmodule.c Modules/_codecsmodule.c Modules/_weakref.c)
add_library(pyloader_python27_compiler STATIC ${_python_sources})
target_include_directories(pyloader_python27_compiler PRIVATE Include Python Parser)
target_compile_definitions(pyloader_python27_compiler PRIVATE
    Py_BUILD_CORE=1 NDEBUG=1 PLATFORM="pyloader-compiler")
target_compile_options(pyloader_python27_compiler PRIVATE
    -fvisibility=hidden -fno-strict-aliasing -Wno-deprecated-declarations
    -Wno-incompatible-pointer-types -Wno-int-conversion)
set_target_properties(pyloader_python27_compiler PROPERTIES
    POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
if(NOT ANDROID)
    target_link_libraries(pyloader_python27_compiler PRIVATE m pthread dl)
else()
    target_link_libraries(pyloader_python27_compiler PRIVATE m dl)
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    target_compile_definitions(pyloader_python27_compiler PRIVATE PYLOADER_COMPILER_HOST_TEST=1)
    add_executable(python27_compiler_host CompilerHost.c)
    target_link_libraries(python27_compiler_host PRIVATE pyloader_python27_compiler)
    add_executable(python27_game_objects_host CompilerGameObjectsHost.c)
    target_link_libraries(python27_game_objects_host PRIVATE pyloader_python27_compiler)
    target_compile_options(python27_game_objects_host PRIVATE -UNDEBUG)
endif()

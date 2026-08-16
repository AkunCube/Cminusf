add_library(
    codegen STATIC
    CodeGen.cpp
    CodeGenRegister.cpp
    Liverange.cpp
    Register.cpp
    Regalloc.cpp
)

target_link_libraries(codegen common IR_lib)

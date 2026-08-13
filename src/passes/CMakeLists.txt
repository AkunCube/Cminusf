add_library(
    opt_lib STATIC
    ConstPropagation.cpp
    Dominators.cpp
    Mem2Reg.cpp
    FuncInfo.cpp
    DeadCode.cpp
    GVN.cpp
    LoopInvHoist.cpp
    LoopSearch.cpp
    UnreachableBlockElim.cpp
)

add_subdirectory(common)

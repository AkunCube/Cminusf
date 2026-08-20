add_library(
    opt_lib STATIC
    ConstPropagation.cpp
    Dominators.cpp
    Mem2Reg.cpp
    GlobalValueNumbering.cpp
    FuncInfo.cpp
    DeadCode.cpp
    GVN.cpp
    LoopInvHoist.cpp
    LoopInfo.cpp
    SimplifyPhi.cpp
    SplitCriticalEdges.cpp
    UnreachableBlockElim.cpp
)

add_subdirectory(common)

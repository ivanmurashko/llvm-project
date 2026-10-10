set(LLVM_LINK_COMPONENTS
  FrontendOpenMP
  Support
  )

include_directories(${LLVM_MAIN_SRC_DIR}/../mlir/include)
include_directories(${CMAKE_BINARY_DIR}/tools/mlir/include)

add_clang_library(clangTidyCIRModule STATIC
  CIRTidyModule.cpp
  Lifetime.cpp

  LINK_LIBS
  clangCIR
  clangTidy
  clangTidyUtils
  MLIRCIRTransforms
  MLIRIR
  MLIRPass
  MLIRSupport

  DEPENDS
  MLIRCIRPassIncGen
  ClangDriverOptions
  )

clang_target_link_libraries(clangTidyCIRModule
  PRIVATE
  clangAST
  clangASTMatchers
  clangBasic
  )

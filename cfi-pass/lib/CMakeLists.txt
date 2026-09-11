add_llvm_library(CFIPass MODULE
  CFIPass.cpp
  PLUGIN_TOOL
  opt
)

target_link_libraries(CFIPass PRIVATE LLVMSupport)

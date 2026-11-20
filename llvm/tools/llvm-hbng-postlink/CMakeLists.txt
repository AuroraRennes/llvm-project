set(LLVM_LINK_COMPONENTS
  Object
  ObjCopy
  Support
  )

add_llvm_tool(llvm-hbng-postlink
  llvm-hbng-postlink.cpp
  )

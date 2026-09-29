define void @target_func() {
  ret void
}

define void @call_directly() {
  call void @target_func()
  ret void
}

define void @call_indirectly() {
  %fp = alloca ptr
  store ptr @target_func, ptr %fp
  %loaded = load ptr, ptr %fp
  call void %loaded()
  ret void
}

define i32 @main() {
entry:
  ret i32 0
}

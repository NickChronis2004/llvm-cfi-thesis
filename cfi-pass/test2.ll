@table = global [2 x ptr] [ptr @f, ptr @g]

define void @f() {
  ret void
}

define i32 @g(i32 %x) {
  ret i32 %x
}

define void @h() {
  ret void
}

define void @caller(ptr %fp) {
  call void %fp()
  call void @h()
  ret void
}

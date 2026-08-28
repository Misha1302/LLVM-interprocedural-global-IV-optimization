@g = internal global i32 1, align 4

; Deliberately unusual defined returns_twice function with no @g accesses.
define internal void @rt() returns_twice nounwind nosync {
entry:
  ret void
}

define i32 @main() {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %v = load i32, ptr @g, align 4
  %n = add i32 %v, 1
  store i32 %n, ptr @g, align 4
  call void @rt()
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 3
  br i1 %more, label %loop, label %exit
exit:
  %r = load i32, ptr @g, align 4
  ret i32 %r
}

@g = internal global i32 7, align 4

define i32 @test(i1 %early) {
entry:
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  %v = load i32, ptr @g, align 4
  br i1 %early, label %exit, label %body
body:
  %n = add i32 %v, 1
  store i32 %n, ptr @g, align 4
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 3
  br i1 %more, label %header, label %exit
exit:
  %r = load i32, ptr @g, align 4
  ret i32 %r
}

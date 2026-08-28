@g = internal global i32 4, align 4

define i32 @test(i1 %early) {
entry:
  br label %header

header:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  br i1 %early, label %exit, label %body

body:
  %v = load i32, ptr @g, align 4
  %n = add i32 %v, 2
  store i32 %n, ptr @g, align 4
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 3
  br i1 %more, label %header, label %exit

exit:
  %r = load i32, ptr @g, align 4
  ret i32 %r
}

define i32 @main() {
entry:
  store i32 4, ptr @g, align 4
  %early_result = call i32 @test(i1 true)

  store i32 4, ptr @g, align 4
  %normal_result = call i32 @test(i1 false)

  %early_ok = icmp eq i32 %early_result, 4
  %normal_ok = icmp eq i32 %normal_result, 10
  %ok = and i1 %early_ok, %normal_ok
  %result = select i1 %ok, i32 0, i32 1
  ret i32 %result
}

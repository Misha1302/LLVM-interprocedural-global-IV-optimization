@g = internal global i32 99, align 4

define i32 @main() {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  store i32 10, ptr @g, align 4
  %x = load i32, ptr @g, align 4
  %y = add i32 %x, %i
  store i32 %y, ptr @g, align 4
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 4
  br i1 %more, label %loop, label %exit
exit:
  %result = load i32, ptr @g, align 4
  ret i32 %result
}

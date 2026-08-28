@g = internal global i32 7, align 4

define i32 @main() {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %old = load i32, ptr @g, align 4
  %new = add i32 %old, 3
  store i32 %new, ptr @g, align 4
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 5
  br i1 %more, label %loop, label %exit

exit:
  %result = load i32, ptr @g, align 4
  ret i32 %result
}

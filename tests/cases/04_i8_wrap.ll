@g = internal global i8 120, align 1

define i32 @main() {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %old = load i8, ptr @g, align 1
  %new = add i8 %old, 100
  store i8 %new, ptr @g, align 1
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 7
  br i1 %more, label %loop, label %exit
exit:
  %result8 = load i8, ptr @g, align 1
  %result = zext i8 %result8 to i32
  ret i32 %result
}

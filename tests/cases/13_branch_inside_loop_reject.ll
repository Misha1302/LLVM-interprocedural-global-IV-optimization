@g = internal global i32 1, align 4

define i32 @main(i1 %choose) {
entry:
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %next1, %left ], [ %next2, %right ]
  br i1 %choose, label %left, label %right
left:
  %a = load i32, ptr @g, align 4
  %b = add i32 %a, 1
  store i32 %b, ptr @g, align 4
  %next1 = add i32 %i, 1
  %c1 = icmp ult i32 %next1, 3
  br i1 %c1, label %header, label %exit
right:
  %c = load i32, ptr @g, align 4
  %d = add i32 %c, 2
  store i32 %d, ptr @g, align 4
  %next2 = add i32 %i, 1
  %c2 = icmp ult i32 %next2, 3
  br i1 %c2, label %header, label %exit
exit:
  %r = load i32, ptr @g, align 4
  ret i32 %r
}

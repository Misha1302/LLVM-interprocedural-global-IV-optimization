@g = internal global i32 1, align 4

define i32 @main(i1 %early) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %cont ]
  %v = load i32, ptr @g, align 4
  %n = add i32 %v, 1
  store i32 %n, ptr @g, align 4
  br i1 %early, label %exit1, label %cont
cont:
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 3
  br i1 %more, label %loop, label %exit2
exit1:
  %a = load i32, ptr @g, align 4
  ret i32 %a
exit2:
  %b = load i32, ptr @g, align 4
  ret i32 %b
}

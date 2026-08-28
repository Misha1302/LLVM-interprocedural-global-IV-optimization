@g = internal global i32 1, align 4

define i32 @main(i1 %which) {
entry:
  br i1 %which, label %p1, label %p2
p1:
  br label %loop
p2:
  br label %loop
loop:
  %i = phi i32 [ 0, %p1 ], [ 0, %p2 ], [ %next, %loop ]
  %v = load i32, ptr @g, align 4
  %n = add i32 %v, 1
  store i32 %n, ptr @g, align 4
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 3
  br i1 %more, label %loop, label %exit
exit:
  %r = load i32, ptr @g, align 4
  ret i32 %r
}

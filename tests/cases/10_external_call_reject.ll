@g = internal global i32 1, align 4

declare void @external() nounwind nosync

define i32 @main() {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %v = load i32, ptr @g, align 4
  %n = add i32 %v, 1
  store i32 %n, ptr @g, align 4
  call void @external()
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 3
  br i1 %more, label %loop, label %exit
exit:
  %r = load i32, ptr @g, align 4
  ret i32 %r
}

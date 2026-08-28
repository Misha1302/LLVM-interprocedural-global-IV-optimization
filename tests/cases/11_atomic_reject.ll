@g = internal global i32 1, align 4

define i32 @main() {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %v = load atomic i32, ptr @g unordered, align 4
  %n = add i32 %v, 1
  store atomic i32 %n, ptr @g unordered, align 4
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 3
  br i1 %more, label %loop, label %exit
exit:
  %r = load atomic i32, ptr @g unordered, align 4
  ret i32 %r
}

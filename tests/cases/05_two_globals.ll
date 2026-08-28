@g = internal global i32 1, align 4
@h = internal global i32 10, align 4

define i32 @main() {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %gv = load i32, ptr @g, align 4
  %hv = load i32, ptr @h, align 4
  %gn = add i32 %gv, 2
  %hn = sub i32 %hv, 1
  store i32 %gn, ptr @g, align 4
  store i32 %hn, ptr @h, align 4
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 5
  br i1 %more, label %loop, label %exit
exit:
  %a = load i32, ptr @g, align 4
  %b = load i32, ptr @h, align 4
  %r = add i32 %a, %b
  ret i32 %r
}

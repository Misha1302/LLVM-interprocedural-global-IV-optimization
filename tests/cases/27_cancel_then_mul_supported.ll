@g = internal global i32 11, align 4

define i32 @main() {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %a = load i32, ptr @g, align 4
  %b = load i32, ptr @g, align 4
  %zero = sub i32 %a, %b
  %c = load i32, ptr @g, align 4
  %n = mul i32 %zero, %c
  store i32 %n, ptr @g, align 4
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 3
  br i1 %more, label %loop, label %exit
exit:
  %r = load i32, ptr @g, align 4
  ret i32 %r
}

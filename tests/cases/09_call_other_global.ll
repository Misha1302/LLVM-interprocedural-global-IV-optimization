@g = internal global i32 2, align 4
@h = internal global i32 20, align 4

define internal void @touch_h() #0 {
entry:
  %v = load i32, ptr @h, align 4
  %n = add i32 %v, 1
  store i32 %n, ptr @h, align 4
  ret void
}

define i32 @main() {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %v = load i32, ptr @g, align 4
  %n = add i32 %v, 3
  store i32 %n, ptr @g, align 4
  call void @touch_h() #0
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 5
  br i1 %more, label %loop, label %exit
exit:
  %a = load i32, ptr @g, align 4
  %b = load i32, ptr @h, align 4
  %r = add i32 %a, %b
  ret i32 %r
}

attributes #0 = { nounwind nosync }

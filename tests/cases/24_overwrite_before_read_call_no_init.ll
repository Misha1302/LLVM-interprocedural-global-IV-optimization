@g = internal global i32 4, align 4
@sink = internal global i32 0, align 4

define internal void @observe() #0 {
entry:
  %v = load i32, ptr @g, align 4
  store i32 %v, ptr @sink, align 4
  ret void
}

define i32 @main() {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  store i32 12, ptr @g, align 4
  call void @observe() #0
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 3
  br i1 %more, label %loop, label %exit
exit:
  %a = load i32, ptr @g, align 4
  %b = load i32, ptr @sink, align 4
  %r = xor i32 %a, %b
  ret i32 %r
}

attributes #0 = { nounwind nosync }

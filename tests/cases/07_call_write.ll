@g = internal global i32 3, align 4

define internal void @bump() #0 {
entry:
  %v = load i32, ptr @g, align 4
  %n = add i32 %v, 7
  store i32 %n, ptr @g, align 4
  ret void
}

define i32 @main() {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %old = load i32, ptr @g, align 4
  %new = mul i32 %old, 2
  store i32 %new, ptr @g, align 4
  call void @bump() #0
  %after = load i32, ptr @g, align 4
  %plus = add i32 %after, 1
  store i32 %plus, ptr @g, align 4
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 3
  br i1 %more, label %loop, label %exit
exit:
  %r = load i32, ptr @g, align 4
  ret i32 %r
}

attributes #0 = { nounwind nosync }

@g = internal global i32 5, align 4

define internal void @leaf() #0 {
entry:
  %v = load i32, ptr @g, align 4
  %n = sub i32 %v, 2
  store i32 %n, ptr @g, align 4
  ret void
}

define internal void @mid() #0 {
entry:
  call void @leaf() #0
  ret void
}

define i32 @main() {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %v = load i32, ptr @g, align 4
  %n = add i32 %v, 10
  store i32 %n, ptr @g, align 4
  call void @mid() #0
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 4
  br i1 %more, label %loop, label %exit
exit:
  %r = load i32, ptr @g, align 4
  ret i32 %r
}

attributes #0 = { nounwind nosync }

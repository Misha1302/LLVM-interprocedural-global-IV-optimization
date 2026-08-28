target datalayout = "e-p:64:64-i32:32"
@g = internal global i32 7, align 1

define i32 @main() {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %v = load i32, ptr @g, align 1
  %n = add i32 %v, 5
  store i32 %n, ptr @g, align 1
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 4
  br i1 %more, label %loop, label %exit
exit:
  %r = load i32, ptr @g, align 1
  ret i32 %r
}

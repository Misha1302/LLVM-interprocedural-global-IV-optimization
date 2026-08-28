target datalayout = "e-p:64:64-p1:64:64-A0"
@g = internal addrspace(1) global i32 1, align 4

define i32 @main() {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %v = load i32, ptr addrspace(1) @g, align 4
  %n = add i32 %v, 1
  store i32 %n, ptr addrspace(1) @g, align 4
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, 3
  br i1 %more, label %loop, label %exit
exit:
  %r = load i32, ptr addrspace(1) @g, align 4
  ret i32 %r
}

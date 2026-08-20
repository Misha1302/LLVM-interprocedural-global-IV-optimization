@g = internal global i32 10

define i32 @foo() {
entry:
  %value = load i32, ptr @g
  ret i32 %value
}
@g = internal global i64 10
@h = internal global i64 100

; g -> g + 2
define void @inc_g() {
entry:
  %x = load i64, ptr @g
  %y = add i64 %x, 2
  store i64 %y, ptr @g
  ret void
}

; g -> 3 * g
define void @triple_g() {
entry:
  %x = load i64, ptr @g
  %y = mul i64 %x, 3
  store i64 %y, ptr @g
  ret void
}

; h -> h - 5
define void @dec_h() {
entry:
  %x = load i64, ptr @h
  %y = sub i64 %x, 5
  store i64 %y, ptr @h
  ret void
}

; Интересный случай:
;
; g:
;   x
;   -> x + 2
;   -> 3(x + 2)
;   -> 3x + 6
;
; Итоговый эффект должен стать:
; g -> 6 + 3*x
define void @update_g() {
entry:
  call void @inc_g()
  call void @triple_g()
  ret void
}

; Два вызова одной функции:
;
; g -> g + 4
define void @inc_twice() {
entry:
  call void @inc_g()
  call void @inc_g()
  ret void
}

; Работает сразу с двумя globals:
;
; g -> g + 2
; h -> h - 5
define void @update_both() {
entry:
  call void @inc_g()
  call void @dec_h()
  ret void
}

; Прямой store + вызов.
;
; В идеале:
;   g -> g + 10
;   затем inc_g
;   g -> g + 12
;
; Этот случай станет интересным,
; когда начнём учитывать порядок store/call.
define void @direct_and_call() {
entry:
  %x = load i64, ptr @g
  %y = add i64 %x, 10
  store i64 %y, ptr @g

  call void @inc_g()
  ret void
}

; ДВА store в одну global.
; Твой текущий анализ должен считать это Unknown.
define void @two_stores() {
entry:
  %x = load i64, ptr @g
  %a = add i64 %x, 1
  store i64 %a, ptr @g

  %y = load i64, ptr @g
  %b = add i64 %y, 1
  store i64 %b, ptr @g

  ret void
}

define i32 @main() {
entry:
  call void @update_g()
  call void @inc_twice()
  call void @update_both()
  ret i32 0
}
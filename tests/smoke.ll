@g = internal global i64 0
@h = internal global i64 10


; ============================================================
; Helper function.
; Effect:
;   g -> g + 2
; ============================================================

define void @add2() {
entry:
  %x = load i64, ptr @g
  %next = add i64 %x, 2
  store i64 %next, ptr @g
  ret void
}


; ============================================================
; TEST 1
;
; Самый простой loop: один BasicBlock.
;
; Одна итерация:
;   g -> g + 5
;
; Ожидаемый путь одной итерации:
;   loop.simple -> loop.simple
;
; Но второй header твоя функция печатать НЕ должна.
; ============================================================

define void @simple_loop(i64 %n) {
entry:
  br label %loop.simple

loop.simple:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop.simple ]

  %g.old = load i64, ptr @g
  %g.next = add i64 %g.old, 5
  store i64 %g.next, ptr @g

  %i.next = add i64 %i, 1
  %continue = icmp slt i64 %i.next, %n

  br i1 %continue,
     label %loop.simple,
     label %exit

exit:
  ret void
}


; ============================================================
; TEST 2
;
; Loop состоит из нескольких BasicBlock:
;
;       header
;        /   \
;      body  exit
;       |
;      latch
;       |
;       +----> header
;
; Одна итерация изменяет:
;   h -> h - 3
;
; Ожидаемый обход:
;   loop.header
;   loop.body
;   loop.latch
; ============================================================

define void @multi_block_loop(i64 %n) {
entry:
  br label %loop.header

loop.header:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop.latch ]

  %continue = icmp slt i64 %i, %n

  br i1 %continue,
     label %loop.body,
     label %exit

loop.body:
  %h.old = load i64, ptr @h
  %h.next = sub i64 %h.old, 3
  store i64 %h.next, ptr @h

  br label %loop.latch

loop.latch:
  %i.next = add i64 %i, 1
  br label %loop.header

exit:
  ret void
}


; ============================================================
; TEST 3
;
; Вызов функции внутри loop.
;
; add2:
;   g -> g + 2
;
; После вызова:
;   g -> g + 3
;
; Итого за одну итерацию:
;   g -> g + 5
;
; Это пригодится, когда начнём применять function summaries
; внутри loop.
; ============================================================

define void @loop_with_call(i64 %n) {
entry:
  br label %loop.call

loop.call:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop.call ]

  call void @add2()

  %g.old = load i64, ptr @g
  %g.next = add i64 %g.old, 3
  store i64 %g.next, ptr @g

  %i.next = add i64 %i, 1
  %continue = icmp slt i64 %i.next, %n

  br i1 %continue,
     label %loop.call,
     label %exit

exit:
  ret void
}


; ============================================================
; TEST 4
;
; Намеренно неподдерживаемый пока случай.
;
; Внутри loop есть развилка:
;
;              /-> left  --\
;   header ---               --> latch --> header
;              \-> right --/
;
; У header ДВА successor'а внутри loop.
;
; Твой текущий алгоритм должен вывести:
;   unsupported
;
; НЕЛЬЗЯ последовательно сложить эффекты left и right,
; потому что за одну итерацию выполняется только одна ветка.
; ============================================================

define void @branch_inside_loop(i1 %flag, i64 %n) {
entry:
  br label %loop.branch

loop.branch:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop.latch ]

  br i1 %flag,
     label %loop.left,
     label %loop.right

loop.left:
  %g.left.old = load i64, ptr @g
  %g.left.next = add i64 %g.left.old, 1
  store i64 %g.left.next, ptr @g

  br label %loop.latch

loop.right:
  %g.right.old = load i64, ptr @g
  %g.right.next = add i64 %g.right.old, 2
  store i64 %g.right.next, ptr @g

  br label %loop.latch

loop.latch:
  %i.next = add i64 %i, 1
  %continue = icmp slt i64 %i.next, %n

  br i1 %continue,
     label %loop.branch,
     label %exit

exit:
  ret void
}
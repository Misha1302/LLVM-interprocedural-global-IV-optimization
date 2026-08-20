## Implementation plan

1. Collect GlobalVariables
2. Get all reads and writes
3. Handle effect for each function on global
    1. g -> g + 1
    2. g -> g - 2
    3. g -> ?
4. Handle functions calls
5. Find effects in loops
6. Evaluate evolution
    1. g(loop_iteration) = G_entry + i * step